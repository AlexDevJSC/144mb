/* Eye remains the ONLY real renderer adapter. This file just registers it. */
#include "../../src/adapters/eye_adapter.h"
BonesSubsystem bones_mod_eye_create(void *eye_context) {
    return bones_eye_subsystem((BonesEye *)eye_context);
}
