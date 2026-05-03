#include "user_state.h"

namespace launcher::d2d {

UserInfo  g_user;
UserStatus g_status = UserStatus::Online;

std::string g_session_token;
std::string g_user_id;

std::wstring g_avatar_path;
std::vector<std::wstring> g_user_tags;
std::mutex g_user_tags_mtx;

std::vector<std::wstring> g_login_log;

}  // namespace launcher::d2d
