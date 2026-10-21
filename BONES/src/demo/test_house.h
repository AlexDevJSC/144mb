#ifndef BONES_TEST_HOUSE_H
#define BONES_TEST_HOUSE_H
#include "../../include/bones/bones.h"
/* Pure visual test fixture, no collision/physics or persistent world state.
   Uses the unchanged public BonesQuad API; frontons are degenerate quads. */
#define BONES_HOUSE_QUADS 10u
bool bones_house_append_scene(BonesScene *scene);
/* Legacy visual test-hook used until a canonical scene-source API is agreed. */
BonesModule bones_test_house_module(void);
#endif
