// 运营/安全事件的日志流可观测性。
//
// 写操作已经落 audit_log 表（持久审计），但默认 TraceLayer 不会把这些业务事件
// 打到日志流，运维排查（grep 日志、告警）看不到。这里补一层 tracing 事件。
//
// 与 audit_log 表互补：表是审计取证，日志是实时可观测。两者都保留。

/// 记录一次运营写操作（operator 增删改、config 改动、封禁、频道策略、邀请码等）。
///
/// `actor` 操作者标识，`action` 动作（与 audit_log.action 一致），`target` 操作对象。
pub fn event(actor: &str, action: &str, target: &str) {
    tracing::info!(
        target: "launcher::audit",
        actor = %actor,
        action = %action,
        operation_target = %target,
        "operational write",
    );
}
