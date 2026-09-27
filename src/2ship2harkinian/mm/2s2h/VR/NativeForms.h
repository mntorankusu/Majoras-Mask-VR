#pragma once
#include "forms.h"
struct Player;
namespace mmvrgame {
bool GiantTransformationActive(Player*);
bool FirstPersonFormAllowed(Player*);
float FormEyeHeight(Player*);
float FormStandingEyeHeight(Player*);
float FloorPinnedWorldScaleTarget(Player* p, float formEye);
void RecordFormEyeHeight(Player*);
bool NativeAbilityOwnsFacing(Player*);
const void* FormHandMesh(Player*, int hand);
} // namespace mmvrgame
