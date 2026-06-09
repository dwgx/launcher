WITH doomed AS (
  SELECT id FROM users WHERE username LIKE :'prefix_like'
), del_sessions AS (
  DELETE FROM sessions WHERE user_id IN (SELECT id FROM doomed) RETURNING 1
), del_tags AS (
  DELETE FROM user_tags WHERE user_id IN (SELECT id FROM doomed) RETURNING 1
), del_reactions AS (
  DELETE FROM message_reactions WHERE user_id IN (SELECT id FROM doomed) RETURNING 1
), del_messages AS (
  DELETE FROM messages WHERE sender_id IN (SELECT id FROM doomed) RETURNING 1
), del_members AS (
  DELETE FROM chat_members WHERE user_id IN (SELECT id FROM doomed) RETURNING 1
), del_dm AS (
  DELETE FROM chat_dm_index
  WHERE user_lo IN (SELECT id FROM doomed) OR user_hi IN (SELECT id FROM doomed)
  RETURNING chat_id
), del_chats AS (
  DELETE FROM chats WHERE created_by IN (SELECT id FROM doomed) OR id IN (SELECT chat_id FROM del_dm) RETURNING 1
), del_reviews AS (
  DELETE FROM market_reviews
  WHERE reviewer_id IN (SELECT id FROM doomed)
     OR listing_id IN (SELECT id FROM market_listings WHERE seller_id IN (SELECT id FROM doomed))
  RETURNING 1
), del_orders AS (
  DELETE FROM market_orders WHERE buyer_id IN (SELECT id FROM doomed) OR seller_id IN (SELECT id FROM doomed) RETURNING 1
), del_ledger AS (
  DELETE FROM credit_ledger WHERE user_id IN (SELECT id FROM doomed) RETURNING 1
), del_listings AS (
  DELETE FROM market_listings WHERE seller_id IN (SELECT id FROM doomed) RETURNING 1
), del_pack_users AS (
  DELETE FROM user_sticker_packs
  WHERE user_id IN (SELECT id FROM doomed)
     OR pack_id IN (SELECT id FROM sticker_packs WHERE creator_id IN (SELECT id FROM doomed))
  RETURNING 1
), del_pack_items AS (
  DELETE FROM sticker_pack_items
  WHERE pack_id IN (SELECT id FROM sticker_packs WHERE creator_id IN (SELECT id FROM doomed))
     OR sticker_id IN (SELECT id FROM stickers WHERE creator_id IN (SELECT id FROM doomed))
  RETURNING 1
), del_packs AS (
  DELETE FROM sticker_packs WHERE creator_id IN (SELECT id FROM doomed) RETURNING 1
), del_stickers AS (
  DELETE FROM stickers WHERE creator_id IN (SELECT id FROM doomed) RETURNING 1
), del_media AS (
  DELETE FROM media_files WHERE uploader_id IN (SELECT id FROM doomed) RETURNING 1
), del_avatars AS (
  DELETE FROM user_avatar_meta WHERE user_id IN (SELECT id FROM doomed) RETURNING 1
), del_login AS (
  DELETE FROM login_history WHERE user_id IN (SELECT id FROM doomed) RETURNING 1
), del_audit AS (
  DELETE FROM audit_log
  WHERE actor IN (SELECT id::text FROM doomed) OR target IN (SELECT id::text FROM doomed)
  RETURNING 1
), del_users AS (
  DELETE FROM users WHERE id IN (SELECT id FROM doomed) RETURNING 1
)
SELECT 'audit_users_deleted=' || COUNT(*) FROM del_users;
SELECT 'audit_users_remaining=' || COUNT(*) FROM users WHERE username LIKE :'prefix_like';
