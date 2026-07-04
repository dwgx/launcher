-- Wave 3: 缩略图 + BlurHash 占位符支持。
--
-- media_files 已有 width/height（0005_chat.sql:81-82），本迁移只补两列：
--   * blurhash   —— 上传时对位图算出的 BlurHash 串（4x3 分量），客户端画模糊占位。
--   * has_thumbs —— 是否已生成兄弟缩略图文件（<sha>_s<slot>.jpg|png）。
--
-- 全部 additive（新列可空 / 有默认），对现网旧行无破坏；旧行 blurhash 为 NULL、
-- has_thumbs=false，下载走原图回退，直到 backfill 补齐（见下）。

ALTER TABLE media_files ADD COLUMN IF NOT EXISTS blurhash   TEXT;
ALTER TABLE media_files ADD COLUMN IF NOT EXISTS has_thumbs BOOLEAN NOT NULL DEFAULT FALSE;

-- ---------------------------------------------------------------------
-- BACKFILL（一次性，迁移不自动执行）：
--   现有 image 类 media_files 行 width/height/blurhash 为 NULL、无缩略图文件。
--   需要一个后台任务遍历 mime LIKE 'image/%' 且 has_thumbs=false 的行：
--     1. 读 media_root/<relative_path> 原图字节
--     2. 走 media_thumb::generate 生成缩略图 + blurhash（GIF 跳过缩放）
--     3. UPDATE media_files SET width=?, height=?, blurhash=?, has_thumbs=? WHERE id=?
--   在 backfill 跑完前，下载端的“变体缺失回退原图”逻辑保证功能不受影响。
--   候选行数查询：
--     SELECT count(*) FROM media_files
--       WHERE mime LIKE 'image/%' AND mime <> 'image/gif' AND has_thumbs = FALSE;
-- ---------------------------------------------------------------------
