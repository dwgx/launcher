#include "user_state.h"
#include "steam.h"

namespace launcher::d2d {

SteamInfo g_steam;
UserInfo  g_user;
UserStatus g_status = UserStatus::Online;

std::string g_session_token;
std::string g_user_id;

std::wstring g_avatar_path;
std::vector<std::wstring> g_user_tags;
std::mutex g_user_tags_mtx;

wchar_t g_geo_country[16] = {0};

std::vector<std::wstring> g_login_log;

}  // namespace launcher::d2d
