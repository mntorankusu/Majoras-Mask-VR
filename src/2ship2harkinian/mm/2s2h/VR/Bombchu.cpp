#ifdef MMVR_ENABLE
#include "Bombchu.h"
#include "ItemUse.h"
#include "FormAim.h"
#include "AimReticle.h"
#include "Interactions.h"
#include "runtime.h"
#include "ui.h"
#include <fstream>
extern "C" {
#include "global.h"
#include "overlays/actors/ovl_En_Bom_Chu/z_en_bom_chu.h"
int MMVR_ReadyThrowable(PlayState*, Player*, int);
void MMVR_PlayerEmptyHands(PlayState*, Player*);
}
namespace mmvrgame {
bool BombchuPlacement(PlayState* play, Player* p, const mmvr::Matrix& head, mmvr::Matrix& target) {
    target = {};
    if (!play || !p || !head.m[3][3])
        return false;
    Vec3f eye{ head.m[3][0], head.m[3][1], head.m[3][2] };
    float x = -head.m[2][0], y = -head.m[2][1], z = -head.m[2][2], xz = std::hypot(x, z);
    if (!std::isfinite(xz) || !std::isfinite(y) || xz < .001f)
        return false;
    x /= xz;
    z /= xz;
    float reach = mmvr::GetSettings().Get(mmvr::Setting::BombchuPlaceReach) * mmvr::WorldUnitsPerMetre();
    float distance = y < -.05f ? std::clamp((eye.y - p->actor.world.pos.y) * xz / -y, 6.f, reach) : reach;
    Vec3f probe{ p->actor.world.pos.x + x * distance, p->actor.world.pos.y + 20, p->actor.world.pos.z + z * distance };
    CollisionPoly* floor = nullptr;
    int bg = BGCHECK_SCENE;
    float height = BgCheck_EntityRaycastFloor5(&play->colCtx, &floor, &bg, &p->actor, &probe);
    if (!floor || COLPOLY_GET_NORMAL(floor->normal.y) < .5f || height - p->actor.world.pos.y > 15 ||
        height - p->actor.world.pos.y < -30)
        return false;
    Vec3f position{ probe.x, height + 1, probe.z }, hit;
    CollisionPoly* wall = nullptr;
    int wallBg = BGCHECK_SCENE;
    // Both the headset and feet must have a clear route to the placement. A floor
    // ray on the far side of a wall does not authorize spawning through that wall.
    Vec3f from{ p->actor.world.pos.x, p->actor.world.pos.y + 5, p->actor.world.pos.z };
    if (BgCheck_EntityLineTest2(&play->colCtx, &eye, &position, &hit, &wall, true, true, true, true, &wallBg,
                                &p->actor) ||
        BgCheck_EntityLineTest2(&play->colCtx, &from, &position, &hit, &wall, true, true, true, true, &wallBg,
                                &p->actor))
        return false;
    target = mmvr::YawPose(std::atan2(x, z), position.x, height, position.z);
    return true;
}
bool HeldBombchu(Player* p) {
    return p && p->heldActor && p->heldActor->id == ACTOR_EN_BOM_CHU && p->heldActor->parent == &p->actor;
}
bool BombchuReticleVisible(Player* p) {
    return p && SelectedItem(gPlayState) == ITEM_BOMBCHU && ItemAllowed(p, ITEM_BOMBCHU) &&
           (!p->heldActor || HeldBombchu(p)) && FormTrackingReady(p) &&
           mmvr::GetSettings().Get(mmvr::Setting::BombchuReticle) > .5f;
}
void UpdateBombchuReticle(mmvr::CameraFrame& frame) {
    auto* play = gPlayState;
    auto* p = play ? GET_PLAYER(play) : nullptr;
    if (!p || SelectedItem(play) != ITEM_BOMBCHU)
        return;
    frame.itemReticle = {};
    if (!BombchuReticleVisible(p))
        return;
    auto head = FormHeadPose();
    mmvr::Matrix target;
    if (!BombchuPlacement(play, p, head, target))
        return;
    Vec3f eye{ head.m[3][0], head.m[3][1], head.m[3][2] };
    XrVector3f direction{ target.m[3][0] - eye.x, target.m[3][1] - eye.y, target.m[3][2] - eye.z };
    float length = std::sqrt(SQ(direction.x) + SQ(direction.y) + SQ(direction.z));
    if (length < 1)
        return;
    direction.x /= length;
    direction.y /= length;
    direction.z /= length;
    frame.itemReticle = AimReticle(play, p, eye, direction, { eye.x, eye.y, eye.z }, length + 5, length);
}
bool PlaceBombchu(PlayState* play, Player* p) {
    if (!p || !HeldBombchu(p) || p->heldActor->init || !ItemAllowed(p, ITEM_BOMBCHU) || !FormTrackingReady(p))
        return false;
    mmvr::Matrix target;
    if (!BombchuPlacement(play, p, FormHeadPose(), target))
        return false;
    // Stock was consumed only once when the trigger readied this actor.
    auto* chu = reinterpret_cast<EnBomChu*>(p->heldActor);
    chu->vrPlacementPending = true;
    chu->vrPlacement = { target.m[3][0], target.m[3][1], target.m[3][2] };
    chu->vrPlacementYaw = static_cast<s16>(std::lround(mmvr::PoseYaw(target) * 32768.f / 3.14159265359f));
    MMVR_NativeThrow(play, p);
    MMVR_PlayerEmptyHands(play, p);
    mmvr::HapticPulse(mmvr::SwordController(mmvr::GetSettings()), .25f);
    if (mmvr::GetSettings().Get(mmvr::Setting::SwordDiagnostics) > .5f)
        std::ofstream("mmvr-combat.log", std::ios::app)
            << "bombchu-place yaw=" << chu->vrPlacementYaw << " pos=" << chu->vrPlacement.x << "," << chu->vrPlacement.y
            << "," << chu->vrPlacement.z << "\n";
    return chu->actor.parent == nullptr;
}
} // namespace mmvrgame
#endif
