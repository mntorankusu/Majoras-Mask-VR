#ifdef MMVR_ENABLE
#include "NativeForms.h"
#include "GoronCombat.h"
#include "FormPresentation.h"
#include "Camera.h"
#include "Interactions.h"
#include <algorithm>
#include <cmath>
#include <fstream>
extern "C" {
#include "global.h"
#include "objects/object_test3/object_test3.h"
void Player_Action_Idle(Player*, PlayState*);
void Player_Action_45(Player*, PlayState*);
void Player_Action_46(Player*, PlayState*);
void Player_Action_47(Player*, PlayState*);
void Player_Action_48(Player*, PlayState*);
void Player_Action_49(Player*, PlayState*);
void Player_Action_51(Player*, PlayState*);
void Player_Action_89(Player*, PlayState*);
void Player_Action_90(Player*, PlayState*);
extern Gfx* gPlayerLeftHandClosedDLs[2 * PLAYER_FORM_MAX];
extern Gfx* gPlayerRightHandClosedDLs[2 * PLAYER_FORM_MAX];
extern Gfx* gPlayerLeftHandOpenDLs[2 * PLAYER_FORM_MAX];
extern Gfx* gPlayerRightHandOpenDLs[2 * PLAYER_FORM_MAX];
}
static_assert(int(mmvr::Form::Human) == PLAYER_FORM_HUMAN && int(mmvr::Form::Deku) == PLAYER_FORM_DEKU &&
              int(mmvr::Form::Goron) == PLAYER_FORM_GORON && int(mmvr::Form::Zora) == PLAYER_FORM_ZORA &&
              int(mmvr::Form::FierceDeity) == PLAYER_FORM_FIERCE_DEITY);
namespace mmvrgame {
bool GiantTransformationActive(Player* p) {
    if (!p)
        return false;
    // Twinmold temporarily grows Link for the cinematic, then shrinks the world
    // and restores Link to .01. Mask state alone cannot describe that transition.
    if (p->actionFunc == Player_Action_89 || p->actionFunc == Player_Action_90)
        return true;
    if (p->currentMask != PLAYER_MASK_GIANT)
        return false;
    return (p->stateFlags1 & PLAYER_STATE1_100) || !std::isfinite(p->actor.scale.x) ||
           !std::isfinite(p->actor.scale.y) || !std::isfinite(p->actor.scale.z) ||
           std::abs(p->actor.scale.x - .01f) > .0001f || std::abs(p->actor.scale.y - .01f) > .0001f ||
           std::abs(p->actor.scale.z - .01f) > .0001f;
}
bool FirstPersonFormAllowed(Player* p) {
    if (!p || !mmvr::ProfileForForm(p->transformation) || GiantTransformationActive(p))
        return false;
    if (p->currentMask == PLAYER_MASK_GIANT && p->transformation != PLAYER_FORM_HUMAN)
        return false;
    return p->transformation == PLAYER_FORM_HUMAN || mmvr::GetSettings().Get(mmvr::Setting::FormFirstPerson) > .5f;
}
namespace {
struct HeadCalibration {
    float sum = 0, low = 10000, high = -10000, height = 0;
    int count = 0;
};
// Stable standing eye anchors in native form order. These are the settled
// head-limb offsets measured from PlayerDrawEnd during the form-lifecycle
// calibration (roughly 83, 59, 60, 24, and 38 game units). They keep the first
// camera frame aligned with the model before the six-frame idle calibration is
// available; in particular, using the Human slider's nominal value (48) here
// would raise the view about 10 units, then visibly pull it down after idle.
constexpr float StandingEyeAnchor[PLAYER_FORM_MAX] = {83.f, 59.f, 60.f, 24.f, 38.f};
HeadCalibration formHeads[PLAYER_FORM_MAX];
Player* headOwner = nullptr;
// Shared eye computation. With applyCompact, Goron shielding/curling uses its
// stable lowered anchor; without it the standing skeleton height is returned
// so world scale never reacts to a defensive crouch.
float EyeHeightForPosture(Player* p, const mmvr::FormProfile* profile, bool applyCompact) {
    // Use the settled six-frame idle calibration whenever available. Before it
    // completes, start from the measured standing model anchor so calibration
    // cannot move the camera when a newly spawned player first becomes idle.
    // Ordinary animation bob remains opt-in. Intentional defensive postures
    // have their own stable eye anchor, independent of that comfort option.
    float model = p == headOwner ? formHeads[p->transformation].height : 0.f;
    if (!(model >= 15.f && model <= 160.f))
        model = StandingEyeAnchor[p->transformation];
    const bool compactPosture = applyCompact && p->transformation == PLAYER_FORM_GORON &&
        ((p->stateFlags3 & PLAYER_STATE3_1000) || (p->stateFlags1 & PLAYER_STATE1_400000));
    if (!compactPosture && mmvr::GetSettings().Get(mmvr::Setting::ExperimentalFirstPersonMotion) > .5f) {
        const float focusHeight = p->actor.focus.pos.y - p->actor.world.pos.y;
        if (std::isfinite(focusHeight) && focusHeight >= 15.f && focusHeight <= 160.f)
            model = focusHeight;
    }
    float eye = mmvr::AdjustedEyeHeight(mmvr::GetSettings(), profile->eyeHeight, model);
    // Match the compact defensive silhouettes without sampling animated focus
    // points. Camera.cpp eases entry/exit at display cadence; calibration stays
    // standing-only, so these offsets cannot become the next standing height.
    if (compactPosture)
        eye = std::max(4.f, eye - (StandingEyeAnchor[PLAYER_FORM_GORON] - 24.f));
    return eye;
}
} // namespace
void RecordFormEyeHeight(Player* p) {
    if (!p || p->transformation >= PLAYER_FORM_MAX)
        return;
    if (headOwner != p) {
        for (auto& h : formHeads)
            h = {};
        headOwner = p;
    }
    auto& h = formHeads[p->transformation];
    if (h.height || p->currentMask == PLAYER_MASK_GIANT ||
        p->actionFunc != Player_Action_Idle || p->heldActor || p->csAction != PLAYER_CSACTION_NONE ||
        !(p->actor.bgCheckFlags & BGCHECKFLAG_GROUND) ||
        std::abs(p->actor.scale.y - (p->transformation == PLAYER_FORM_FIERCE_DEITY ? .015f : .01f)) > .0001f)
        return;
    // Native head-limb focus point is defined by the model, above the neck joint.
    // Sample only a settled standing pose; never follow animation bob or rolling.
    float y = p->actor.focus.pos.y - p->actor.world.pos.y;
    if (!std::isfinite(y) || y < 15 || y > 160)
        return;
    h.low = std::min(h.low, y);
    h.high = std::max(h.high, y);
    h.sum += y;
    if (++h.count == 6) {
        if (h.high - h.low < 6) {
            h.height = h.sum / h.count;
            std::ofstream("mmvr-form-heights.log", std::ios::app)
                << "form=" << int(p->transformation) << " modelEye=" << h.height
                << " nativeHeight=" << Player_GetHeight(p) << "\n";
        } else
            h = {};
    }
}
float FormEyeHeight(Player* p) {
    if (!p)
        return 52.f;
    auto* profile = mmvr::ProfileForForm(p->transformation);
    if (!profile)
        return 52.f;
    return EyeHeightForPosture(p, profile, true);
}
// Standing eye without the defensive-crouch anchor. World scale derives from
// the skeleton size, so shielding/curling as Goron keeps its lowered view
// without growing the world.
float FormStandingEyeHeight(Player* p) {
    if (!p)
        return 52.f;
    auto* profile = mmvr::ProfileForForm(p->transformation);
    if (!profile)
        return 52.f;
    return EyeHeightForPosture(p, profile, false);
}
// Target world scale for the floor-pinned mode (1 when the option is off).
// Scale is chosen so the scaled floor-to-eye distance matches the configured
// Player Height: scale = formEye / (40 * heightMetres).
float FloorPinnedWorldScaleTarget(Player* p, float formEye) {
    if (!p || !mmvr::FloorPinnedWorldScaleActive(mmvr::GetSettings()) || GiantTransformationActive(p))
        return 1.f;
    const float heightMetres = mmvr::GetSettings().Get(mmvr::Setting::PlayerHeight);
    if (!(heightMetres > 0.2f))
        return 1.f;
    return mmvr::WorldScaleForEyes(formEye, 40.f * heightMetres);
}
bool NativeAbilityOwnsFacing(Player* p) {
    return p->actionFunc == Player_Action_45 || p->actionFunc == Player_Action_46 ||
           p->actionFunc == Player_Action_47 || p->actionFunc == Player_Action_48 ||
           p->actionFunc == Player_Action_49 || p->actionFunc == Player_Action_51 ||
           (p->stateFlags2 & PLAYER_STATE2_100) || MMVR_DekuFlowerStage(p) || MMVR_DekuSpinning(p) ||
           (p->stateFlags1 & PLAYER_STATE1_200000) ||
           (p->transformation == PLAYER_FORM_GORON && (p->stateFlags3 & PLAYER_STATE3_1000)) ||
           (p->transformation == PLAYER_FORM_ZORA && (p->stateFlags3 & PLAYER_STATE3_8000)) ||
           (p->transformation == PLAYER_FORM_DEKU && (p->stateFlags3 & PLAYER_STATE3_100));
}
const void* FormHandMesh(Player* p, int hand) {
    if (MMVR_ControlledKafei(p))
        return hand ? gKafeiRightHandDL : gKafeiLeftHandDL;
    if (!mmvr::ProfileForForm(p->transformation))
        return nullptr;
    if (p->transformation == PLAYER_FORM_FIERCE_DEITY && hand == 0 &&
        Player_GetMeleeWeaponHeld(p) == PLAYER_MELEEWEAPON_SWORD_TWO_HANDED)
        return MMVR_TrackedLeftHandMesh(p);
    if (p->transformation == PLAYER_FORM_HUMAN)
        return hand ? MMVR_TrackedRightHandMesh(p) : MMVR_TrackedLeftHandMesh(p);
    if (GoronFists(p) || (hand == 0 && p->leftHandType == PLAYER_MODELTYPE_LH_BOTTLE))
        return hand ? gPlayerRightHandClosedDLs[p->transformation * 2]
                    : gPlayerLeftHandClosedDLs[p->transformation * 2];
    return hand ? gPlayerRightHandOpenDLs[p->transformation * 2] : gPlayerLeftHandOpenDLs[p->transformation * 2];
}
} // namespace mmvrgame
#endif
