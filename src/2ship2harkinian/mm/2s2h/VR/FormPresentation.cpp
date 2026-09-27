#ifdef MMVR_ENABLE
#include "FormPresentation.h"
#include "TransformationEffects.h"
#include "FormAim.h"
#include "Camera.h"
#include "NativeForms.h"
#include "NativeCombat.h"
#include "AimReticle.h"
#include "form_presentation.h"
#include "runtime.h"
#include "ui.h"
#include "2s2h/Enhancements/FrameInterpolation/FrameInterpolation.h"
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <libultraship/libultraship.h>
#include <fast/resource/type/DisplayList.h>
extern "C" {
#include "global.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include "objects/object_link_zora/object_link_zora.h"
}
namespace {
bool Immersive(Player* p) {
    return p && gPlayState && p == GET_PLAYER(gPlayState) && mmvr::FirstPersonRequested() &&
           mmvrgame::FirstPersonFormAllowed(p);
}
bool FinAway(Player* p, int hand) {
    if (!(p->stateFlags1 & PLAYER_STATE1_ZORA_BOOMERANG_THROWN))
        return false;
    auto* a = p->zoraBoomerangActor;
    if (!a)
        return true;
    return a->params == hand || (a->child && a->child->params == hand) || (a->parent && a->parent->params == hand);
}
} // namespace
namespace mmvrgame {
bool FormReticleVisible(Player* p) {
    if (!Immersive(p) || !FormTrackingReady(p))
        return false;
    int stage = MMVR_FormAimStage(p);
    if (stage != 1 && stage != 2)
        return false;
    return (p->transformation == PLAYER_FORM_DEKU && mmvr::GetSettings().Get(mmvr::Setting::DekuReticle) > .5f) ||
           (p->transformation == PLAYER_FORM_ZORA && mmvr::GetSettings().Get(mmvr::Setting::ZoraReticle) > .5f);
}
void UpdateFormPresentation(mmvr::CameraFrame& frame) {
    auto* p = gPlayState ? GET_PLAYER(gPlayState) : nullptr;
    auto head = FormHeadPose();
    frame.formEffectAnchor = p && MMVR_TransformationStyle(p)              ? head
                             : p && p->transformation == PLAYER_FORM_GORON ? mmvr::FormEffectAnchor(head)
                                                                           : mmvr::Matrix{};
    if (!Immersive(p))
        return;
    if (mmvr::GetSettings().Get(mmvr::Setting::SwordDiagnostics) > .5f) {
        int stage = MMVR_FormAimStage(p),
            roll = (p->transformation == PLAYER_FORM_GORON && (p->stateFlags3 & PLAYER_STATE3_1000)) != 0,
            spin = MMVR_DekuSpinning(p);
        static Player* lastPlayer = nullptr;
        static int lastScene = -1, lastForm = -1, lastStage = -1, lastRoll = -1, lastSpin = -1;
        static unsigned lastFrame = 0;
        static void* lastAction = nullptr;
        if ((void*)p->actionFunc != lastAction || p != lastPlayer || gPlayState->sceneId != lastScene ||
            p->transformation != lastForm || stage != lastStage || roll != lastRoll || spin != lastSpin ||
            gPlayState->gameplayFrames - lastFrame >= 30) {
            static std::ofstream log("mmvr-forms.log", std::ios::app);
            static bool session = []() {
                log << "session-start\n";
                return true;
            }();
            lastFrame = gPlayState->gameplayFrames;
            log << "form-state frame=" << gPlayState->gameplayFrames << " scene=" << gPlayState->sceneId
                << " form=" << int(p->transformation) << " stage=" << stage << " roll=" << roll << " spin=" << spin
                << " ready=" << FormTrackingReady(p) << " reticle=" << FormReticleVisible(p)
                << " held=" << int(p->heldItemAction) << " aimState=" << int(p->unk_AA5)
                << " action=" << (void*)p->actionFunc << " upper=" << (void*)p->upperActionFunc
                << " flags=" << p->stateFlags1 << "," << p->stateFlags2 << "," << p->stateFlags3
                << " transforming=" << MMVR_TransformationStyle(p) << " body=" << p->actor.world.pos.x << ","
                << p->actor.world.pos.y << "," << p->actor.world.pos.z << " maskClaim=" << mmvr::MaskTriggerClaimed()
                << " controller=" << CONTROLLER1(&gPlayState->state)->cur.button
                << " stick=" << int(CONTROLLER1(&gPlayState->state)->cur.stick_x) << ","
                << int(CONTROLLER1(&gPlayState->state)->cur.stick_y) << " head=" << head.m[3][0] << "," << head.m[3][1]
                << "," << head.m[3][2] << "\n"
                << std::flush;
            lastPlayer = p;
            lastScene = gPlayState->sceneId;
            lastForm = p->transformation;
            lastAction = (void*)p->actionFunc;
            lastStage = stage;
            lastRoll = roll;
            lastSpin = spin;
        }
    }
    if (p->transformation == PLAYER_FORM_ZORA && !(p->stateFlags2 & PLAYER_STATE2_USING_OCARINA))
        for (int h = 0; h < 2; ++h)
            if (!FinAway(p, h))
                frame.formFins[h] = (h == 1 - mmvr::SwordController(mmvr::GetSettings()) && ShieldRaised())
                                        ? ShieldModelPose()
                                        : mmvr::AttachedFin(frame.hands[h], h, mmvr::GetSettings());
    if (!FormReticleVisible(p))
        return;
    Vec3f position{};
    Vec3s rotation{};
    int hand = p->transformation == PLAYER_FORM_DEKU ? -1 : 0;
    if (!MMVR_FormProjectilePose(gPlayState, p, hand, &position.x, &rotation.x))
        return;
    XrVector3f direction{ -head.m[2][0], -head.m[2][1], -head.m[2][2] };
    // Zora fins share a gaze-parallel heading. The central marker is the aim direction;
    // native target seeking and return arcs still operate after launch.
    if (p->transformation == PLAYER_FORM_ZORA) {
        position = { head.m[3][0] + direction.x * 8, head.m[3][1] + direction.y * 8, head.m[3][2] + direction.z * 8 };
    }
    frame.itemReticle =
        AimReticle(gPlayState, p, position, direction, { head.m[3][0], head.m[3][1], head.m[3][2] }, 1000, 400);
    float size = mmvr::GetSettings().Get(mmvr::Setting::FormReticleSize);
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            frame.itemReticle.m[row][col] *= size;
}
} // namespace mmvrgame
static void DrawNativeFins(PlayState* play) {
    auto* p = GET_PLAYER(play);
    if (!Immersive(p) || p->transformation != PLAYER_FORM_ZORA || (p->stateFlags2 & PLAYER_STATE2_USING_OCARINA))
        return;
    OPEN_DISPS(play->state.gfxCtx);
    Gfx_SetupDL25_Opa(play->state.gfxCtx);
    // No inherited culling inversion from a body, item, shield, or water reflection.
    gSPClearExtraGeometryMode(POLY_OPA_DISP++, G_EX_INVERT_CULLING);
    for (int h = 0; h < 2; ++h) {
        if (FinAway(p, h))
            continue;
        auto* matrix = (Mtx*)GRAPH_ALLOC(play->state.gfxCtx, sizeof(Mtx));
        std::memset(matrix, 0, sizeof(Mtx));
        mmvr::SetFormFinMatrix(h, matrix);
        gSPMatrix(POLY_OPA_DISP++, matrix, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_OPA_DISP++,
                       (Gfx*)((h == 1 - mmvr::SwordController(mmvr::GetSettings()) && mmvrgame::ShieldRaised())
                                  ? object_link_zora_DL_0110A8
                              : h ? gameplay_keep_DL_06FF68
                                  : gameplay_keep_DL_06FE20));
    }
    CLOSE_DISPS(play->state.gfxCtx);
}
namespace mmvrgame {
void DrawFormFins(PlayState* play) {
    DrawNativeFins(play);
}
} // namespace mmvrgame
extern "C" float MMVR_DekuTrailOpacity(const void* effect) {
    auto* p = gPlayState ? GET_PLAYER(gPlayState) : nullptr;
    if (!p || p->transformation != PLAYER_FORM_DEKU || !mmvr::FirstPersonRequested()) return 1.f;
    for (int i = 0; i < 2; ++i)
        if (p->meleeWeaponEffectIndex[i] >= 0 && effect == Effect_GetByIndex(p->meleeWeaponEffectIndex[i]))
            return mmvr::GetSettings().Get(mmvr::Setting::DekuSpinOpacity) * .01f;
    return 1.f;
}
extern "C" void MMVR_DekuSpinTrail(PlayState* play, Player* p, float* tip, float* base) {
    if (!Immersive(p) || !MMVR_DekuSpinning(p) || !tip || !base)
        return;
    auto head = mmvrgame::FormHeadPose();
    if (!head.m[3][3])
        return;
    float angle = float(p->actor.shape.rot.y) * 3.141592654f / 32768.f;
    float radius = mmvr::GetSettings().Get(mmvr::Setting::DekuSpinRadius) * 120;
    // Preserve the native spin phase, trail material and lifetime. Widen its ribbon.
    for (int i = 0; i < 2; ++i) {
        float* point = i ? base : tip;
        float r = radius * (i ? .45f : 1.2f);
        point[0] = head.m[3][0] + std::sin(angle) * r;
        point[1] = head.m[3][1] - 4 + (i ? -2.f : 2.f);
        point[2] = head.m[3][2] + std::cos(angle) * r;
    }
}
extern "C" int MMVR_GoronEffectMatrix(PlayState*, Player* p) {
    if (!Immersive(p) || p->transformation != PLAYER_FORM_GORON)
        return false;
    auto anchor = mmvr::FormEffectAnchor(mmvrgame::FormHeadPose());
    if (!anchor.m[3][3])
        return false;
    Matrix_Put((MtxF*)&anchor);
    float scale = mmvr::GetSettings().Get(mmvr::Setting::GoronEffectRadius) * mmvr::WorldUnitsPerMetre() / 3575.f;
    Matrix_Translate(0, -5, 0, MTXMODE_APPLY);
    Matrix_RotateYS(0x4000, MTXMODE_APPLY);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    return true;
}
extern "C" const void* MMVR_FormEffectList(const char* path) {
    if (!MMVR_FirstPersonBody())
        return path;
    const char* originalPath = path;
    if (std::strncmp(path, "__OTR__", 7) == 0)
        path += 7;
    struct Entry {
        std::shared_ptr<Fast::DisplayList> source;
        std::vector<Gfx> commands;
    };
    static std::unordered_map<std::string, Entry> cache;
    auto source = std::dynamic_pointer_cast<Fast::DisplayList>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(path));
    if (!source)
        return originalPath;
    auto& entry = cache[path];
    if (entry.source != source) {
        entry.source = source;
        entry.commands = source->Instructions;
        // Inside-view meshes can be seen from either side. Patch a cached copy
        // so the source resource and user texture packs remain unchanged.
        for (auto& g : entry.commands)
            if ((g.words.w0 >> 24) == G_GEOMETRYMODE)
                g.words.w1 &= ~G_CULL_BOTH;
    }
    return entry.commands.data();
}
extern "C" void MMVR_RegisterFormEffect(const void* address) {
    if (!MMVR_FirstPersonBody())
        return;
    auto anchor = mmvr::FormEffectAnchor(mmvrgame::FormHeadPose());
    if (!anchor.m[3][3])
        return;
    MtxF native;
    Matrix_Get(&native);
    mmvr::Matrix model;
    std::memcpy(&model, &native, sizeof(model));
    mmvr::SetFormEffectMatrix(address, mmvr::Multiply(model, mmvr::InversePose(anchor)));
}
#endif
