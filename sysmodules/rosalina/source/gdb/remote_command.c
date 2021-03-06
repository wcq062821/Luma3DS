/*
*   This file is part of Luma3DS
*   Copyright (C) 2016-2017 Aurora Wright, TuxSH
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
*       * Requiring preservation of specified reasonable legal notices or
*         author attributions in that material or in the Appropriate Legal
*         Notices displayed by works containing it.
*       * Prohibiting misrepresentation of the origin of that material,
*         or requiring that modified versions of such material be marked in
*         reasonable ways as different from the original version.
*/

#include "gdb/remote_command.h"
#include "gdb/net.h"
#include "csvc.h"
#include "fmt.h"
#include "gdb/breakpoints.h"

struct
{
    const char *name;
    GDBCommandHandler handler;
} remoteCommandHandlers[] =
{
    { "syncrequestinfo"   , GDB_REMOTE_COMMAND_HANDLER(SyncRequestInfo) },
    { "translatehandle"   , GDB_REMOTE_COMMAND_HANDLER(TranslateHandle) },
    { "getmmuconfig"      , GDB_REMOTE_COMMAND_HANDLER(GetMmuConfig) },
    { "flushcaches"       , GDB_REMOTE_COMMAND_HANDLER(FlushCaches) },
    { "toggleextmemaccess", GDB_REMOTE_COMMAND_HANDLER(ToggleExternalMemoryAccess) },
};

static const char *GDB_SkipSpaces(const char *pos)
{
    const char *nextpos;
    for(nextpos = pos; *nextpos != 0 && ((*nextpos >= 9 && *nextpos <= 13) || *nextpos == ' '); nextpos++);
    return nextpos;
}

GDB_DECLARE_REMOTE_COMMAND_HANDLER(SyncRequestInfo)
{
    char outbuf[GDB_BUF_LEN / 2 + 1];
    Result r;
    int n;

    if(ctx->commandData[0] != 0)
        return GDB_ReplyErrno(ctx, EILSEQ);

    if(ctx->selectedThreadId == 0)
        ctx->selectedThreadId = ctx->currentThreadId;

    if(ctx->selectedThreadId == 0)
    {
        n = sprintf(outbuf, "Cannot run this command without a selected thread.\n");
        goto end;
    }

    u32 id;
    u32 cmdId;
    ThreadContext regs;
    u32 instr;
    Handle process;
    r = svcOpenProcess(&process, ctx->pid);
    if(R_FAILED(r))
    {
        n = sprintf(outbuf, "Invalid process (wtf?)\n");
        goto end;
    }

    for(id = 0; id < MAX_DEBUG_THREAD && ctx->threadInfos[id].id != ctx->selectedThreadId; id++);

    r = svcGetDebugThreadContext(&regs, ctx->debug, ctx->selectedThreadId, THREADCONTEXT_CONTROL_CPU_REGS);

    if(R_FAILED(r) || id == MAX_DEBUG_THREAD)
    {
        n = sprintf(outbuf, "Invalid or running thread.\n");
        goto end;
    }

    r = svcReadProcessMemory(&cmdId, ctx->debug, ctx->threadInfos[id].tls + 0x80, 4);
    if(R_FAILED(r))
    {
        n = sprintf(outbuf, "Invalid or running thread.\n");
        goto end;
    }

    r = svcReadProcessMemory(&instr, ctx->debug, regs.cpu_registers.pc, (regs.cpu_registers.cpsr & 0x20) ? 2 : 4);

    if(R_SUCCEEDED(r) && (((regs.cpu_registers.cpsr & 0x20) && instr == BREAKPOINT_INSTRUCTION_THUMB) || instr == BREAKPOINT_INSTRUCTION_ARM))
    {
        u32 savedInstruction;
        if(GDB_GetBreakpointInstruction(&savedInstruction, ctx, regs.cpu_registers.pc) == 0)
            instr = savedInstruction;
    }

    if(R_FAILED(r) || ((regs.cpu_registers.cpsr & 0x20) && !(instr == 0xDF32 || (instr == 0xDFFE && regs.cpu_registers.r[12] == 0x32)))
                   || (!(regs.cpu_registers.cpsr & 0x20) && !(instr == 0xEF000032 || (instr == 0xEF0000FE && regs.cpu_registers.r[12] == 0x32))))
    {
        n = sprintf(outbuf, "The selected thread is not currently performing a sync request (svc 0x32).\n");
        goto end;
    }

    char name[12];
    Handle handle;
    r = svcCopyHandle(&handle, CUR_PROCESS_HANDLE, (Handle)regs.cpu_registers.r[0], process);
    if(R_FAILED(r))
    {
        n = sprintf(outbuf, "Invalid handle.\n");
        goto end;
    }

    r = svcControlService(SERVICEOP_GET_NAME, name, handle);
    if(R_FAILED(r))
        name[0] = 0;

    n = sprintf(outbuf, "%s 0x%x, 0x%08x\n", name, cmdId, ctx->threadInfos[id].tls + 0x80);

end:
    svcCloseHandle(handle);
    svcCloseHandle(process);
    return GDB_SendHexPacket(ctx, outbuf, n);
}

GDB_DECLARE_REMOTE_COMMAND_HANDLER(TranslateHandle)
{
    bool ok;
    u32 val;
    char *end;
    int n;
    Result r;
    u32 kernelAddr;
    Handle handle, process;
    s64 refcountRaw;
    u32 refcount;
    char classBuf[32], serviceBuf[12] = { 0 };
    char outbuf[GDB_BUF_LEN / 2 + 1];

    if(ctx->commandData[0] == 0)
        return GDB_ReplyErrno(ctx, EILSEQ);

    val = xstrtoul(ctx->commandData, &end, 0, true, &ok);

    if(!ok)
        return GDB_ReplyErrno(ctx, EILSEQ);

    end = (char *)GDB_SkipSpaces(end);

    if(*end != 0)
        return GDB_ReplyErrno(ctx, EILSEQ);

    r = svcOpenProcess(&process, ctx->pid);
    if(R_FAILED(r))
    {
        n = sprintf(outbuf, "Invalid process (wtf?)\n");
        goto end;
    }

    r = svcCopyHandle(&handle, CUR_PROCESS_HANDLE, (Handle)val, process);
    if(R_FAILED(r))
    {
        n = sprintf(outbuf, "Invalid handle.\n");
        goto end;
    }

    svcTranslateHandle(&kernelAddr, classBuf, handle);
    svcGetHandleInfo(&refcountRaw, handle, 1);
    svcControlService(SERVICEOP_GET_NAME, serviceBuf, handle);
    refcount = (u32)(refcountRaw - 1);
    if(serviceBuf[0] != 0)
        n = sprintf(outbuf, "(%s *)0x%08x /* %s handle, %u %s */\n", classBuf, kernelAddr, serviceBuf, refcount, refcount == 1 ? "reference" : "references");
    else
        n = sprintf(outbuf, "(%s *)0x%08x /* %u %s */\n", classBuf, kernelAddr, refcount, refcount == 1 ? "reference" : "references");

end:
    svcCloseHandle(handle);
    svcCloseHandle(process);
    return GDB_SendHexPacket(ctx, outbuf, n);
}

extern bool isN3DS;
GDB_DECLARE_REMOTE_COMMAND_HANDLER(GetMmuConfig)
{
    int n;
    char outbuf[GDB_BUF_LEN / 2 + 1];
    Result r;
    Handle process;

    if(ctx->commandData[0] != 0)
        return GDB_ReplyErrno(ctx, EILSEQ);

    r = svcOpenProcess(&process, ctx->pid);
    if(R_FAILED(r))
        n = sprintf(outbuf, "Invalid process (wtf?)\n");
    else
    {
        s64 TTBCR, TTBR0;
        svcGetSystemInfo(&TTBCR, 0x10002, 0);
        svcGetProcessInfo(&TTBR0, process, 0x10008);
        n = sprintf(outbuf, "TTBCR = %u\nTTBR0 = 0x%08x\nTTBR1 =", (u32)TTBCR, (u32)TTBR0);
        for(u32 i = 0; i < (isN3DS ? 4 : 2); i++)
        {
            s64 TTBR1;
            svcGetSystemInfo(&TTBR1, 0x10002, 1 + i);

            if(i == (isN3DS ? 3 : 1))
                n += sprintf(outbuf + n, " 0x%08x\n", (u32)TTBR1);
            else
                n += sprintf(outbuf + n, " 0x%08x /", (u32)TTBR1);
        }
        svcCloseHandle(process);
    }

    return GDB_SendHexPacket(ctx, outbuf, n);
}

GDB_DECLARE_REMOTE_COMMAND_HANDLER(FlushCaches)
{
    if(ctx->commandData[0] != 0)
        return GDB_ReplyErrno(ctx, EILSEQ);

    svcFlushEntireDataCache();
    svcInvalidateEntireInstructionCache();

    return GDB_ReplyOk(ctx);
}

GDB_DECLARE_REMOTE_COMMAND_HANDLER(ToggleExternalMemoryAccess)
{
    int n;
    char outbuf[GDB_BUF_LEN / 2 + 1];

    ctx->enableExternalMemoryAccess = !ctx->enableExternalMemoryAccess;

    n = sprintf(outbuf, "External memory access %s successfully.\n", ctx->enableExternalMemoryAccess ? "enabled" : "disabled");

    return GDB_SendHexPacket(ctx, outbuf, n);
}

GDB_DECLARE_QUERY_HANDLER(Rcmd)
{
    char commandData[GDB_BUF_LEN / 2 + 1];
    char *endpos;
    const char *errstr = "Unrecognized command.\n";
    u32 len = strlen(ctx->commandData);

    if(len == 0 || (len % 2) == 1 || GDB_DecodeHex(commandData, ctx->commandData, len / 2) != len / 2)
        return GDB_ReplyErrno(ctx, EILSEQ);
    commandData[len / 2] = 0;

    for(endpos = commandData; !(*endpos >= 9 && *endpos <= 13) && *endpos != ' ' && *endpos != 0; endpos++);

    char *nextpos = (char *)GDB_SkipSpaces(endpos);
    *endpos = 0;

    for(u32 i = 0; i < sizeof(remoteCommandHandlers) / sizeof(remoteCommandHandlers[0]); i++)
    {
        if(strcmp(commandData, remoteCommandHandlers[i].name) == 0)
        {
            ctx->commandData = nextpos;
            return remoteCommandHandlers[i].handler(ctx);
        }
    }

    return GDB_SendHexPacket(ctx, errstr, strlen(errstr));
}
