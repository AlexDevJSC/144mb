#ifndef SKIN_MOUTH_SESSION_BRIDGE_H
#define SKIN_MOUTH_SESSION_BRIDGE_H
/* Optional static host integration. Include this header ONLY if the
 * Mouth 0.4 Session API v1 is installed. Does not compile/network in Skin. */
#include "skin_modkit.h"
#include "../mouth/mouth_lan.h"

static inline int32_t bones_skin_ui_refresh_mouth(const MouthLan *mouth,
                                                   int world_identity_known) {
    MouthSessionInfo info={0};
    MouthSessionResult rc;
    info.size=(uint32_t)sizeof(info);
    rc=mouth_session_get_status(mouth,&info);
    if(rc!=MOUTH_SESSION_OK)
        return bones_skin_ui_lan_status(MOUTH_SESSION_OFFLINE,(unsigned)rc,0u,world_identity_known);
    return bones_skin_ui_lan_status((unsigned)info.phase,(unsigned)info.error,
                                    (unsigned)info.port,world_identity_known);
}
#endif
