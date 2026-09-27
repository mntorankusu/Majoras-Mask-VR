#ifdef MMVR_ENABLE
#include "FairyComfort.h"
#include "Camera.h"
#include "runtime.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <fast/resource/type/DisplayList.h>
extern "C" {
#include "global.h"
}
namespace {
bool spawningCompanionTrail = false;
bool Companion(PlayState* play, Actor* actor) {
    return actor && ((play && GET_PLAYER(play) && GET_PLAYER(play)->tatlActor == actor) ||
                    (actor->id == ACTOR_DM_CHAR00 && actor->params == 0));
}
float Distance(PlayState* play, const float* position) {
    float eye[3];
    if (!position || mmvr::GetSettings().Get(mmvr::Setting::FairyNearComfort) < .5f ||
        !MMVR_EnvironmentEye(play, eye)) return 100000.f;
    const float x = position[0]-eye[0], y = position[1]-eye[1], z = position[2]-eye[2];
    return std::sqrt(x*x+y*y+z*z);
}
// One foot in game units, at the same units-per-metre as tracked hands/head.
inline float FootInUnits() {
    return .3048f * mmvr::WorldUnitsPerMetre();
}
}
extern "C" int MMVR_FairyReplacedByCutscene(PlayState* play, Actor* actor) {
    if (!play || !actor || actor->id != ACTOR_EN_ELF || !GET_PLAYER(play) ||
        GET_PLAYER(play)->tatlActor != actor || play->csCtx.state == CS_STATE_IDLE ||
        !Cutscene_IsCueInChannel(play, CS_CMD_ACTOR_CUE_113)) return false;
    // Authored Tatl owns her shot; Tael (params 1) and collectible fairies remain untouched.
    for (int category = 0; category < ACTORCAT_MAX; ++category)
        for (Actor* replacement = play->actorCtx.actorLists[category].first; replacement; replacement = replacement->next)
            if (replacement->id == ACTOR_DM_CHAR00 && replacement->params == 0 &&
                replacement->update && replacement->draw) return true;
    return false;
}
extern "C" float MMVR_FairyOpacity(PlayState* play, Actor* actor) {
    if (!Companion(play, actor)) return 1.f;
    float fade = std::clamp((Distance(play, &actor->world.pos.x)-FootInUnits())/FootInUnits(), 0.f, 1.f);
    fade = fade*fade*(3.f-2.f*fade);
    return .5f + .5f*fade;
}
extern "C" int MMVR_FairyTrailHidden(PlayState* play, const float* position) {
    // Hide already-live sparkles too: suppressing only new emission leaves a trail across the eyes.
    // Include the visible sparkle footprint, rather than testing its centre only.
    auto* player = play ? GET_PLAYER(play) : nullptr;
    if (player && MMVR_ControlledKafei(player)) return true;
    if (Distance(play, position) <= 2.f*FootInUnits()) return true;
    return player && player->tatlActor && Distance(play, &player->tatlActor->world.pos.x) <= 2.f*FootInUnits();
}
extern "C" void MMVR_FairyTrailBegin(PlayState* play, Actor* actor) {
    auto* player = play ? GET_PLAYER(play) : nullptr;
    spawningCompanionTrail = !(player && MMVR_ControlledKafei(player)) && Companion(play, actor);
}
extern "C" void MMVR_FairyTrailEnd(void) { spawningCompanionTrail = false; }
extern "C" int MMVR_FairyTrailSpawning(void) { return spawningCompanionTrail; }
extern "C" void* MMVR_FairyWingDisplayList(PlayState* play, Actor* actor, int limb, void* original) {
    const float opacity=MMVR_FairyOpacity(play,actor);
    if(!original || opacity>=1.f || limb<2 || limb>5) return original;
    static constexpr const char* paths[]={
        "objects/gameplay_keep/gameplay_keep_DL_029990", "objects/gameplay_keep/gameplay_keep_DL_029A58",
        "objects/gameplay_keep/gameplay_keep_DL_029B20", "objects/gameplay_keep/gameplay_keep_DL_029BE8"};
    auto resource=std::dynamic_pointer_cast<Fast::DisplayList>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(paths[limb-2]));
    if(!resource || resource->Instructions.empty() || resource->Instructions.size()>256) return original;
    auto* copy=static_cast<Gfx*>(GRAPH_ALLOC(play->state.gfxCtx,resource->Instructions.size()*sizeof(Gfx)));
    if(!copy) return original;
    std::memcpy(copy,resource->Instructions.data(),resource->Instructions.size()*sizeof(Gfx));
    for(size_t i=0;i<resource->Instructions.size();++i) {
        auto& command=copy[i];
        const auto opcode=uint8_t(command.words.w0>>24);
        if(opcode==G_MARKER || opcode==G_SETTIMG_OTR_HASH || opcode==G_VTX_OTR_HASH || opcode==G_DL_OTR_HASH) { ++i; continue; }
        if(opcode==G_SETPRIMCOLOR)
            command.words.w1=(command.words.w1 & ~uintptr_t(255)) | uint8_t((command.words.w1 & 255)*opacity);
    }
    return copy;
}
#endif
