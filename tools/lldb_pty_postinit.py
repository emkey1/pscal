import lldb


_EXPRS = [
    "session_id",
    "(void *)pty_master",
    "(void *)pty_slave",
    "(void *)stdio_ctx",
    "stdio_ctx ? ((VProcSessionStdio *)stdio_ctx)->session_id : 0",
    "stdio_ctx ? (void *)((VProcSessionStdio *)stdio_ctx)->pty_master : (void *)0",
    "stdio_ctx ? (void *)((VProcSessionStdio *)stdio_ctx)->pty_slave : (void *)0",
    "stdio_ctx ? ((VProcSessionStdio *)stdio_ctx)->pty_active : false",
    "stdio_ctx ? ((VProcSessionStdio *)stdio_ctx)->pty_out_fd : -1",
    "(size_t)gVProcSessionPtys.count",
    "gVProcSessionPtys.count > 0 ? gVProcSessionPtys.items[0].session_id : 0",
    "gVProcSessionPtys.count > 0 ? (void *)gVProcSessionPtys.items[0].pty_master : (void *)0",
    "gVProcSessionPtys.count > 0 ? (void *)gVProcSessionPtys.items[0].pty_slave : (void *)0",
    "gVProcSessionPtys.count > 0 ? (void *)gVProcSessionPtys.items[0].output_handler : (void *)0",
    "gVProcSessionPtys.count > 1 ? gVProcSessionPtys.items[1].session_id : 0",
    "gVProcSessionPtys.count > 1 ? (void *)gVProcSessionPtys.items[1].pty_master : (void *)0",
    "gVProcSessionPtys.count > 1 ? (void *)gVProcSessionPtys.items[1].pty_slave : (void *)0",
    "gVProcSessionPtys.count > 1 ? (void *)gVProcSessionPtys.items[1].output_handler : (void *)0",
    "gVProcSessionPtys.count > 2 ? gVProcSessionPtys.items[2].session_id : 0",
    "gVProcSessionPtys.count > 2 ? (void *)gVProcSessionPtys.items[2].pty_master : (void *)0",
    "gVProcSessionPtys.count > 2 ? (void *)gVProcSessionPtys.items[2].pty_slave : (void *)0",
    "gVProcSessionPtys.count > 2 ? (void *)gVProcSessionPtys.items[2].output_handler : (void *)0",
]


def _eval_expr(frame, expr, options):
    value = frame.EvaluateExpression(expr, options)
    if not value.IsValid() or value.error.Fail():
        err = value.error.GetCString() if value.error else "unknown"
        print(f"{expr} = <error: {err}>")
        return
    rendered = value.GetValue()
    if rendered is None:
        rendered = value.GetSummary()
    if rendered is None:
        rendered = "<unavailable>"
    print(f"{expr} = {rendered}")


def postinit_callback(frame, _bp_loc, _internal_dict):
    tid = frame.GetThread().GetThreadID()
    print(f"lldb-pty-postinit: hit tid={tid}")
    options = lldb.SBExpressionOptions()
    options.SetLanguage(lldb.eLanguageTypeC)
    for expr in _EXPRS:
        _eval_expr(frame, expr, options)
    return True


def install(debugger):
    target = debugger.GetSelectedTarget()
    if not target or not target.IsValid():
        print("lldb-pty-postinit: no target selected")
        return

    for bp in target.breakpoint_iter():
        if bp.MatchesName("pty-postinit"):
            target.BreakpointDelete(bp.GetID())

    bp = target.BreakpointCreateByLocation("src/ios/vproc.c", 5995)
    bp.AddName("pty-postinit")
    bp.SetScriptCallbackFunction("lldb_pty_postinit.postinit_callback")
    print(f"lldb-pty-postinit: breakpoint {bp.GetID()} set at src/ios/vproc.c:5995")


def __lldb_init_module(debugger, _internal_dict):
    install(debugger)
