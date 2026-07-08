-- 0020: 消息撤回 + 反应媒体（sticker/GIF）+ 撤回时间窗全局配置。
--
-- 全部 additive：新列可空、种子配置用 ON CONFLICT DO NOTHING，绝不重写旧行。
--
--   * messages.recalled_at        —— 撤回时间戳。NULL = 未撤回。
--       与 deleted_at 区分：撤回后行仍可见（history/sync 不过滤），客户端渲染
--       “X 撤回了一条消息”墓碑；deleted_at 才是永久软删除并从历史中隐藏。
--   * message_reactions.sticker_ref —— 反应引用的贴纸 id / 媒体 URL。NULL = 纯 emoji 反应。
--       注意 message_reactions 主键是 (message_id, user_id, emoji)，sticker_ref 不入主键；
--       贴纸反应的去重键仍走 emoji 列（写入端把 sticker_ref 值同时塞进 emoji 列作为判别键），
--       因此同一用户对同一消息可有多个不同贴纸反应，emoji 路径不受影响。
--   * limits.recall_window_secs   —— 撤回时间窗（秒），默认 30。撤回端点运行时读取；
--       管理员可经 /admin/config 修改。expose_to_client=FALSE（后端策略，不下发客户端）。

ALTER TABLE messages          ADD COLUMN IF NOT EXISTS recalled_at TIMESTAMPTZ;
ALTER TABLE message_reactions ADD COLUMN IF NOT EXISTS sticker_ref TEXT;

INSERT INTO app_config_entries
    (key, category, label, description, value_type, value, enabled, expose_to_client, updated_by)
VALUES
    ('limits.recall_window_secs', 'limits', '消息撤回时间窗（秒）',
     '用户可撤回自己消息的时间窗，超过则不允许撤回；单位秒，默认 30',
     'number', '30'::jsonb, TRUE, FALSE, 'migration')
ON CONFLICT (key) DO NOTHING;
