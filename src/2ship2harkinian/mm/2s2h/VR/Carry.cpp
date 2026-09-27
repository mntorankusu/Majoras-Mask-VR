#ifdef MMVR_ENABLE
#include "Carry.h"
#include "solid_hull.h"
#include "ItemUse.h"
#include "Interactions.h"
#include "NativeCombat.h"
#include "Bow.h"
#include "runtime.h"
#include "ui.h"
#include <array>
#include <fstream>
#include <cstring>
extern "C" {
#include "global.h"
#include "overlays/actors/ovl_Obj_Tsubo/z_obj_tsubo.h"
#include "overlays/actors/ovl_En_Ishi/z_en_ishi.h"
#include "overlays/actors/ovl_En_Niw/z_en_niw.h"
#include "overlays/actors/ovl_Obj_Kibako/z_obj_kibako.h"
#include "overlays/actors/ovl_En_Dg/z_en_dg.h"
#include "overlays/actors/ovl_En_Bombf/z_en_bombf.h"
#include "overlays/actors/ovl_En_Bom/z_en_bom.h"
#include "overlays/actors/ovl_Arms_Hook/z_arms_hook.h"
char* ResourceMgr_LoadVtxArrayByName(const char*);
size_t ResourceMgr_GetVtxArraySizeByName(const char*);
void ArmsHook_Wait(ArmsHook*,PlayState*);
void Player_DestroyHookshot(Player*);
}
namespace {
struct Offer {
    Actor* actor = nullptr;
    unsigned frame = 0;
};
std::array<Offer, 64> offers{};
PlayState* context = nullptr;
int scene = -1;
Actor* carried = nullptr;
Player* carrier = nullptr;
unsigned grabbedAt = 0;
int holdingHand = 1;
mmvr::Matrix relative{};
bool Live(PlayState* play, Actor* target) {
    for (auto& list : play->actorCtx.actorLists)
        for (auto* a = list.first; a; a = a->next)
            if (a == target)
                return a->update && !a->init && !a->parent;
    return false;
}
void Context(PlayState* play) {
    if (context != play || scene != play->sceneId) {
        offers = {};
        carried = nullptr;
        carrier = nullptr;
        context = play;
        scene = play->sceneId;
    }
}
const Cylinder16* CarryCylinder(Actor* a) {
    if (a->id == ACTOR_OBJ_TSUBO)
        return &reinterpret_cast<ObjTsubo*>(a)->cylinderCollider.dim;
    if (a->id == ACTOR_EN_NIW)
        return &reinterpret_cast<EnNiw*>(a)->collider.dim;
    if (a->id == ACTOR_EN_ISHI)
        return &reinterpret_cast<EnIshi*>(a)->collider.dim;
    if (a->id == ACTOR_OBJ_KIBAKO)
        return &reinterpret_cast<ObjKibako*>(a)->collider.dim;
    if (a->id == ACTOR_EN_DG)
        return &reinterpret_cast<EnDg*>(a)->collider.dim;
    if (a->id == ACTOR_EN_BOMBF)
        return &reinterpret_cast<EnBombf*>(a)->colliderCylinder.dim;
    return nullptr;
}
// The native rock cylinder is floor-based, while its mesh is centered at
// its origin. Holding by the cylinder midpoint leaves the visible mesh a foot
// below the palm. These centers are measured from the original draw-list
// vertices; texture replacements remain entirely native.
Vec3f HeldModelCenter(Actor* actor) {
    if (actor->id == ACTOR_EN_ISHI) {
        const Vec3f local = (actor->params & 1) ? Vec3f{ -3.f, 1.f, 15.f } : Vec3f{ 1.f, 0.f, 8.5f };
        return { local.x * actor->scale.x, local.y * actor->scale.y, local.z * actor->scale.z };
    }
    if (auto* cylinder = CarryCylinder(actor))
        return { 0, float(cylinder->yShift) + cylinder->height * .5f, 0 };
    return { 0, 10, 0 };
}
mmvr::Matrix GripAttachment(Actor* actor) {
    const auto center = HeldModelCenter(actor);
    // Silver boulders span about 2.4 metres. Centering one at the palm puts
    // the headset inside its backface-culled mesh. Grip the near surface;
    // retain its native size and rotate around that contact point.
    const float surface = actor->id == ACTOR_EN_ISHI && (actor->params & 1) ? 116.f * actor->scale.z : 0.f;
    return mmvr::YawPose(0, -center.x, -center.y, -center.z - surface);
}
// Use the same solid surface used by grab reach, preserving the actual side
// touched rather than snapping every rock or pot to a fixed center offset.
Vec3f ContactPoint(Actor* actor, const mmvr::Matrix& palm) {
    Vec3f point{palm.m[3][0],palm.m[3][1],palm.m[3][2]};
    Vec3f surface;bool inside=false;
    if(mmvrgame::CarryPropSurface(actor,point,surface,inside))return surface;
    if(auto* dim=CarryCylinder(actor)) {
        const float bottom=actor->world.pos.y+dim->yShift;
        point.y=std::clamp(point.y,bottom,bottom+float(dim->height));
        const float x=point.x-actor->world.pos.x,z=point.z-actor->world.pos.z;
        const float radius=std::max(0.f,float(dim->radius)),distance=std::hypot(x,z);
        if(distance>radius && distance>0) {
            point.x=actor->world.pos.x+x*radius/distance;
            point.z=actor->world.pos.z+z*radius/distance;
        }
    } else {
        // Native pickup offers without a solid cylinder use their visible center.
        const auto center=HeldModelCenter(actor);
        point={actor->world.pos.x+center.x,actor->world.pos.y+center.y,actor->world.pos.z+center.z};
    }
    return point;
}
mmvr::Matrix ContactRelative(Actor* actor,const mmvr::Matrix& palm) {
    const auto contact=ContactPoint(actor,palm);
    MtxF matrix;
    Matrix_Push();
    Matrix_SetTranslateRotateYXZ(actor->world.pos.x,actor->world.pos.y,actor->world.pos.z,&actor->shape.rot);
    Matrix_Get(&matrix);
    Matrix_Pop();
    mmvr::Matrix object;std::memcpy(&object,&matrix,sizeof(object));
    for(int c=0;c<3;++c)object.m[3][c]+=object.m[1][c]*actor->shape.yOffset*actor->scale.y;
    const float touched[3]={contact.x,contact.y,contact.z};
    return mmvr::ContactAttachment(object,palm,touched);
}
Vec3f GrabPoint(Actor* a) {
    float height = 10;
    if (auto* dim = CarryCylinder(a))
        height = dim->yShift + dim->height * .5f;
    return { a->world.pos.x, a->world.pos.y + height, a->world.pos.z };
}
} // namespace
// GI_NONE also describes Mikau's scripted pushing interaction. Only native
// liftable actor types participate; their live offers still govern availability.
extern "C" int MMVR_CarryActorSupported(Actor* actor) {
    if (!actor) return false;
    switch (actor->id) {
        case ACTOR_EN_ISHI: case ACTOR_OBJ_TSUBO: case ACTOR_EN_MM:
        case ACTOR_EN_NIW: case ACTOR_OBJ_KIBAKO: case ACTOR_OBJ_FLOWERPOT:
        case ACTOR_OBJ_SNOWBALL2: case ACTOR_EN_DG: case ACTOR_EN_BOMBF:
        case ACTOR_EN_BOM: case ACTOR_EN_KUSA: case ACTOR_EN_KUSA2:
        case ACTOR_OBJ_GRASS_CARRY: return true;
        default: return false;
    }
}
extern "C" void MMVR_RecordCarryOffer(PlayState* play, Actor* actor) {
    if (!play || !MMVR_CarryActorSupported(actor) || !mmvr::FirstPersonRequested())
        return;
    Context(play);
    if (actor->parent || actor->init)
        return;
    for (auto& offer : offers)
        if (offer.actor == actor) {
            offer.frame = play->gameplayFrames;
            return;
        }
    for (auto& offer : offers)
        if (!offer.actor || play->gameplayFrames - offer.frame > 2) {
            offer = { actor, play->gameplayFrames };
            return;
        }
}
namespace mmvrgame {
bool CarryPropSurface(Actor* actor,const Vec3f& point,Vec3f& surface,bool& inside,float contactRadius) {
    if(!actor || !gPlayState)return false;
    int kind=-1;
    if(actor->id==ACTOR_EN_ISHI)kind=(actor->params&1)?0:((actor->params&8)?2:1);
    else if(actor->id==ACTOR_OBJ_TSUBO) {
        static constexpr int pots[]={3,4,5,3};kind=pots[OBJ_TSUBO_GET_TYPE(actor)];
    } else if(actor->id==ACTOR_EN_MM)kind=6;
    else if(actor->id==ACTOR_OBJ_KIBAKO)kind=KIBAKO_BANK_INDEX(actor)?8:7;
    else if(actor->id==ACTOR_OBJ_FLOWERPOT)kind=9;
    else if(actor->id==ACTOR_OBJ_SNOWBALL2)kind=10;
    if(kind<0)return false;
    static const char* paths[]={
        "__OTR__objects/gameplay_field_keep/gameplay_field_keepVtx_006028",
        "__OTR__objects/gameplay_field_keep/gameplay_field_keepVtx_006500",
        "__OTR__objects/object_ishi/object_ishiVtx_000000",
        "__OTR__objects/gameplay_dangeon_keep/gameplay_dangeon_keepVtx_017AE0",
        "__OTR__objects/object_racetsubo/object_racetsuboVtx_000000",
        "__OTR__objects/object_tsubo/object_tsuboVtx_001400",
        "__OTR__objects/gameplay_keep/gameplay_keepVtx_055440",
        "__OTR__objects/gameplay_dangeon_keep/gameplay_dangeon_keepVtx_007710",
        "__OTR__objects/object_kibako/object_kibakoVtx_001000",
        "__OTR__objects/object_flowerpot/object_flowerpotVtx_001000",
        "__OTR__objects/object_goroiwa/object_goroiwaVtx_008490",
    };
    static std::array<mmvr::SolidHull,11> hulls;
    static std::array<bool,11> loaded{};
    static PlayState* loadedPlay=nullptr;static int loadedScene=-1;
    if(loadedPlay!=gPlayState || loadedScene!=gPlayState->sceneId) {
        loaded={};loadedPlay=gPlayState;loadedScene=gPlayState->sceneId;
    }
    auto& hull=hulls[kind];
    if(!loaded[kind]) {
        const size_t count=ResourceMgr_GetVtxArraySizeByName(paths[kind]);
        auto* vertices=reinterpret_cast<Vtx*>(ResourceMgr_LoadVtxArrayByName(paths[kind]));
        std::vector<mmvr::HandPoint> points;
        if(vertices && count>=4 && count<=512)for(size_t i=0;i<count;++i)
            points.push_back({float(vertices[i].v.ob[0]),float(vertices[i].v.ob[1]),float(vertices[i].v.ob[2])});
        hull.Build(points);loaded[kind]=true;
    }
    if(actor->scale.x<=0||actor->scale.y<=0||actor->scale.z<=0)return false;
    MtxF native;Matrix_Push();
    Matrix_SetTranslateRotateYXZ(actor->world.pos.x,actor->world.pos.y,actor->world.pos.z,&actor->shape.rot);
    Matrix_Get(&native);Matrix_Pop();
    mmvr::Matrix pose;std::memcpy(&pose,&native,sizeof(pose));
    for(int c=0;c<3;++c)pose.m[3][c]+=pose.m[1][c]*actor->shape.yOffset*actor->scale.y;
    auto inverse=mmvr::InversePose(pose);
    const auto local=mmvr::Multiply(mmvr::YawPose(0,point.x,point.y,point.z),inverse);
    mmvr::HandPoint query{},closest{};
    for(int c=0;c<3;++c)query[c]=local.m[3][c]/(&actor->scale.x)[c];
    if(!hull.Closest(query,closest,inside,contactRadius<0?-1:contactRadius/std::min({actor->scale.x,actor->scale.y,actor->scale.z})))return false;
    surface={pose.m[3][0],pose.m[3][1],pose.m[3][2]};
    for(int c=0;c<3;++c)for(int axis=0;axis<3;++axis)
        (&surface.x)[c]+=closest[axis]*(&actor->scale.x)[axis]*pose.m[axis][c];
    return true;
}
float CarryGrabSeparation(Actor* actor, const Vec3f& hand) {
    if (!actor || !std::isfinite(hand.x) || !std::isfinite(hand.y) || !std::isfinite(hand.z))
        return INFINITY;
    Vec3f surface;bool inside=false;
    if(CarryPropSurface(actor,hand,surface,inside))
        return inside?0.f:std::sqrt(SQ(hand.x-surface.x)+SQ(hand.y-surface.y)+SQ(hand.z-surface.z));
    auto center = GrabPoint(actor);
    float radial = std::hypot(hand.x - center.x, hand.z - center.z), vertical = std::abs(hand.y - center.y);
    if (auto* cylinder = CarryCylinder(actor)) {
        const auto& dim = *cylinder;
        // Distance to the whole solid cylinder, including its top and bottom caps.
        // The existing setting is an extra hand margin, not a center-only target.
        radial = std::max(0.f, radial - std::max(0.f, float(dim.radius)));
        vertical = std::max(0.f, vertical - std::max(0.f, float(dim.height)) * .5f);
    }
    return std::hypot(radial, vertical);
}
bool CarriedObject(Player* p) {
    return p && gPlayState == context && gPlayState && gPlayState->sceneId == scene && p == carrier && p->heldActor &&
           p->heldActor == carried && p->heldActor->parent == &p->actor;
}
bool CarryReady(PlayState* play, Player* p) {
    return CarriedObject(p) && play->gameplayFrames > grabbedAt;
}
int CarryHand(Player* p) {
    return CarriedObject(p) ? holdingHand : mmvr::SwordController(mmvr::GetSettings());
}
void AdoptNativeCarry(PlayState* play, Player* p) {
    if (!play || !p || CarriedObject(p) || !p->heldActor || p->heldActor->parent != &p->actor ||
        !InteractionsEligible(play, p) || mmvr::GetSettings().Get(mmvr::Setting::PhysicalCarry) < .5f)
        return;
    auto* a = p->heldActor;
    if (!MMVR_CarryActorSupported(a) || a->id == ACTOR_EN_BOM)
        return;
    Context(play);
    // Plucking a bomb flower replaces its rooted actor with a live bomb.
    // Preserve the physical hand across native actor replacement.
    const bool replacedFlower = carrier == p && a->id == ACTOR_EN_BOMBF && Live(play, carried) &&
                                carried->id == ACTOR_EN_BOMBF;
    const int hand = replacedFlower ? holdingHand : mmvr::SwordController(mmvr::GetSettings());
    auto sample=SampleHandThrow(play,p,hand);
    carrier = p;
    carried = a;
    holdingHand = hand;
    grabbedAt = play->gameplayFrames;
    // Native A-button lifts share the tracked carry path. Anchor the object's
    // center at the palm, never the native overhead Goron animation position.
    relative = sample.valid ? ContactRelative(a,CarryPalmPose(sample.pose,holdingHand)) : GripAttachment(a);
    a->shape.yOffset = 0;
}
bool TryGrabCarry(PlayState* play, Player* p, int controller, bool preview) {
    if (controller < 0)
        controller = mmvr::SwordController(mmvr::GetSettings());
    const bool idleHook = p && MMVR_IndependentHookshot(p) && p->heldActor &&
        p->heldActor->id==ACTOR_ARMS_HOOK && !p->heldActor->init && p->heldActor->parent==&p->actor &&
        reinterpret_cast<ArmsHook*>(p->heldActor)->actionFunc==ArmsHook_Wait && !p->actor.parent;
    if (!play || !p || p->transformation == PLAYER_FORM_DEKU || mmvr::GetSettings().Get(mmvr::Setting::PhysicalCarry) < .5f || !mmvr::PhysicalActionsAllowed() ||
        !InteractionsEligible(play, p) || (p->heldActor && !idleHook) ||
        (mmvr::HeldMaskItem() >= 0 && mmvr::HeldMaskController() == controller))
        return false;
    int item = SelectedItem(play);
    // A selected mask is not a held object until its trigger grab starts.
    bool selectedMask = item >= ITEM_MASK_DEKU && item <= ITEM_MASK_GIANT;
    const int equipmentHand = BowHeld() ? mmvr::OffhandController(mmvr::GetSettings()) : mmvr::SwordController(mmvr::GetSettings());
    if ((p->heldItemAction != PLAYER_IA_NONE || (item != ITEM_NONE && !selectedMask)) && controller == equipmentHand)
        return false;
    if (controller == mmvr::OffhandController(mmvr::GetSettings()) && ShieldRaised()) return false;
    Context(play);
    auto sample = SampleHandThrow(play, p, controller);
    if (!sample.valid)
        return false;
    const auto palm=CarryPalmPose(sample.pose,controller);
    const Vec3f hand{ palm.m[3][0], palm.m[3][1], palm.m[3][2] };
    Actor* best = nullptr;
    float nearest = mmvr::GetSettings().Get(mmvr::Setting::CarryGrabDistance) * mmvr::WorldUnitsPerMetre();
    for (auto& offer : offers) {
        if (!offer.actor || play->gameplayFrames - offer.frame > 2 || !Live(play, offer.actor))
            continue;
        // A forbidden heavy object must not steal the nearest valid light offer,
        // including the preview used to arbitrate mask grabs.
        if (p->transformation != PLAYER_FORM_GORON &&
            ((offer.actor->id == ACTOR_EN_ISHI && (offer.actor->params & 1)) ||
             offer.actor->id == ACTOR_EN_MM ||
             (offer.actor->id == ACTOR_EN_BOM && reinterpret_cast<EnBom*>(offer.actor)->isPowderKeg)))
            continue;
        auto center = ContactPoint(offer.actor,palm);
        float distance = CarryGrabSeparation(offer.actor, hand);
        if (!std::isfinite(distance) || distance > nearest)
            continue;
        CollisionPoly* poly = nullptr;
        int bg = BGCHECK_SCENE;
        Vec3f hit;
        auto from = hand;
        if (BgCheck_EntityLineTest2(&play->colCtx, &from, &center, &hit, &poly, true, true, true, true, &bg, &p->actor))
            continue;
        nearest = distance;
        best = offer.actor;
    }
    if (!best) return false;
    if (preview) return true;
    // Native heldActor has one owner. Retire only a fully retracted hook; keep
    // its hand model/action, then recreate its projectile after releasing the prop.
    const auto attachment=ContactRelative(best,palm);
    if (idleHook) Player_DestroyHookshot(p);
    if (!MMVR_AttachCarryActor(play, p, best)) {
        if (idleHook) RestoreSelectedEquipment(play);
        return false;
    }
    carrier = p;
    carried = best;
    holdingHand = controller;
    grabbedAt = play->gameplayFrames;
    // Snapshot before native attachment can rewrite actor rotation/position.
    relative = attachment;
    best->shape.yOffset = 0;
    best->bgCheckFlags = 0;
    best->colChkInfo.displacement = {};
    mmvr::HapticPulse(controller, .25f);
    if (mmvr::GetSettings().Get(mmvr::Setting::SwordDiagnostics) > .5f)
        std::ofstream("mmvr-combat.log", std::ios::app)
            << "carry-grab actor=" << best->id << " hand=" << controller << " distance=" << nearest << "\n";
    return true;
}
bool CarryPose(PlayState* play, Player* p, const mmvr::Matrix& grip, mmvr::Matrix& target) {
    if (!CarriedObject(p))
        return false;
    auto* a = p->heldActor;
    target = mmvr::Multiply(relative, CarryPalmPose(grip,holdingHand));
    Vec3f next{ target.m[3][0], target.m[3][1], target.m[3][2] }, before = a->world.pos, resolved = before;
    CollisionPoly* wall = nullptr;
    int bg = BGCHECK_SCENE;
    float radius = 12, halfHeight = 12;
    if (auto* dim = CarryCylinder(a)) {
        radius = std::max(6.f, float(dim->radius));
        halfHeight = dim->height * .5f;
    }
    BgCheck_EntitySphVsWall3(&play->colCtx, &resolved, &next, &before, radius, &wall, &bg, a, halfHeight);
    Vec3f probe{ resolved.x, resolved.y + halfHeight, resolved.z };
    auto floor = BgCheck_EntityRaycastFloor5(&play->colCtx, &wall, &bg, a, &probe);
    if (wall && floor > resolved.y)
        resolved.y = floor;
    for (int k = 0; k < 3; ++k)
        target.m[3][k] = (&resolved.x)[k];
    // ContactRelative stores the visible model root. Native held actions may
    // change their draw offset (notably snowballs), so return the compensating
    // actor origin; drawing adds the offset back exactly once.
    for (int c = 0; c < 3; ++c)
        target.m[3][c] -= target.m[1][c] * a->shape.yOffset * a->scale.y;
    return true;
}
} // namespace mmvrgame
#endif

#if defined(MMVR_ENABLE) && defined(MMVR_STATE_NATIVE_BACKEND)
#include "NativeStateFields.h"
extern "C" void MMVR_VisitVrCarryState(MMVR_StateSink* sink) {
    mmvrgame::NativeStateField(sink,"vr/carry/offers",offers);
    mmvrgame::NativeStateField(sink,"vr/carry/context",context);
    mmvrgame::NativeStateField(sink,"vr/carry/scene",scene);
    mmvrgame::NativeStateField(sink,"vr/carry/carried",carried);
    mmvrgame::NativeStateField(sink,"vr/carry/carrier",carrier);
    mmvrgame::NativeStateField(sink,"vr/carry/grabbedAt",grabbedAt);
    mmvrgame::NativeStateField(sink,"vr/carry/holdingHand",holdingHand);
    mmvrgame::NativeStateField(sink,"vr/carry/relative",relative);
    for(auto& offer:offers)sink->pointer(sink->context,&offer.actor,0,"vr/carry/offers.actor");
}
#endif
