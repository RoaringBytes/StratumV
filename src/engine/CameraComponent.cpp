// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── CameraComponent registration TU ─────────────────────────────────
// Sole TU that expands SV_REPLICATE / SV_COMPONENT_AUTHORITY for the
// CameraComponent component. The file-scope static inside SV_REPLICATE
// runs at program start; `ensureCameraComponentRegistered()` below is
// the non-inline anchor the server and lab harness call from main() to
// pull this .obj out of the static archive.

#include "CameraComponent.h"

namespace sv {

SV_REPLICATE(CameraComponent,
    SV_FIELD(fovDeg),
    SV_FIELD(aspect),
    SV_FIELD(nearPlane),
    SV_FIELD(farPlane));

SV_COMPONENT_AUTHORITY(CameraComponent, Authority::Editor);

// Non-inline anchor — see CameraComponent.h for why this exists and
// why it must re-apply setAuthority every time it runs. Same two-step
// pattern ensureLightComponentRegistered uses for Authority::Editor,
// lifted verbatim.
const ReplicationMeta& ensureCameraComponentRegistered() {
    const ReplicationMeta& meta =
        sv_buildReplicationMetaFor(static_cast<CameraComponent*>(nullptr));
    ReplicationRegistry::get().setAuthority("CameraComponent", Authority::Editor);
    return meta;
}

} // namespace sv
