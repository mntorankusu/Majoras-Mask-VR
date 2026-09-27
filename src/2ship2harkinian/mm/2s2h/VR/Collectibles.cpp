#ifdef MMVR_ENABLE
#include "Collectibles.h"
#include "FormAim.h"
#include "runtime.h"
extern "C" {
#include "global.h"
}
extern "C" int MMVR_HandCollectible(PlayState* play, Actor* item) {
    if (!play || play != gPlayState || !item || item->id != ACTOR_EN_ITEM00 || item->parent || !item->update ||
        !mmvrgame::FormTrackingReady(GET_PLAYER(play)) || mmvr::GetSettings().Get(mmvr::Setting::HandPickup) < .5f)
        return false;
    auto* drop = reinterpret_cast<EnItem00*>(item);
    if (drop->unk14C > 0)
        return false;
    auto head = mmvrgame::FormHeadPose();
    Vec3f eye{ head.m[3][0], head.m[3][1], head.m[3][2] };
    const auto& dim = drop->collider.dim;
    float height = std::max(0.f, float(dim.height)), radius = std::max(0.f, float(dim.radius));
    Vec3f center{ item->world.pos.x, item->world.pos.y + dim.yShift + height * .5f, item->world.pos.z };
    auto wall = [&](Vec3f a, Vec3f b) {
        Vec3f hit;
        CollisionPoly* poly = nullptr;
        int bg = BGCHECK_SCENE;
        return BgCheck_EntityLineTest2(&play->colCtx, &a, &b, &hit, &poly, true, true, true, true, &bg,
                                       &GET_PLAYER(play)->actor) != 0;
    };
    for (int hand = 0; hand < 2; ++hand) {
        auto pose = mmvrgame::FormHandPose(hand);
        if (!pose.m[3][3])
            continue;
        Vec3f palm{ pose.m[3][0], pose.m[3][1], pose.m[3][2] };
        float horizontal = std::max(0.f, std::hypot(palm.x - center.x, palm.z - center.z) - radius),
              vertical = std::max(0.f, std::abs(palm.y - center.y) - height * .5f);
        if (std::hypot(horizontal, vertical) > mmvr::GetSettings().Get(mmvr::Setting::HandPickupRadius) * mmvr::WorldUnitsPerMetre())
            continue;
        if (std::sqrt(SQ(palm.x - eye.x) + SQ(palm.y - eye.y) + SQ(palm.z - eye.z)) >
                mmvr::GetSettings().Get(mmvr::Setting::AimReach) * mmvr::WorldUnitsPerMetre() ||
            wall(eye, palm) || wall(palm, center))
            continue;
        return true;
    }
    return false;
}
#endif
