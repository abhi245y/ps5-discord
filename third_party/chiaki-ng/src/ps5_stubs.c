// Stand-ins for the PSN holepunch pieces a LAN session never calls.
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <chiaki/remote/holepunch.h>

CHIAKI_EXPORT ChiakiHolepunchRegistInfo chiaki_get_regist_info(ChiakiHolepunchSession session)
{
    ChiakiHolepunchRegistInfo info;
    memset(&info, 0, sizeof info);
    return info;
}
CHIAKI_EXPORT void chiaki_get_ps_selected_addr(ChiakiHolepunchSession session, char *ps_ip) { ps_ip[0] = 0; }
CHIAKI_EXPORT uint16_t chiaki_get_ps_ctrl_port(ChiakiHolepunchSession session) { return 0; }
CHIAKI_EXPORT chiaki_socket_t *chiaki_get_holepunch_sock(ChiakiHolepunchSession session, ChiakiHolepunchPortType type) { return NULL; }
CHIAKI_EXPORT ChiakiErrorCode holepunch_session_create_offer(ChiakiHolepunchSession session) { return CHIAKI_ERR_UNINITIALIZED; }
CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_punch_hole(ChiakiHolepunchSession session, ChiakiHolepunchPortType port_type)
{
    return CHIAKI_ERR_UNINITIALIZED;
}
CHIAKI_EXPORT void chiaki_holepunch_session_fini(ChiakiHolepunchSession session) {}
