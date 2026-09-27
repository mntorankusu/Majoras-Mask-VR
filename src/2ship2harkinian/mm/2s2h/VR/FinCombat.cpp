#ifdef MMVR_ENABLE
#include "FinCombat.h"
#include "FormAim.h"
#include "FormPresentation.h"
#include "NativeCombat.h"
#include "combat.h"
#include "runtime.h"
#include "ui.h"
#include "fixed_history.h"
#include <chrono>
#include <fstream>
extern "C" void CollisionCheck_AC(PlayState*, CollisionCheckContext*, Collider*);
namespace {
struct Sample {
    double time;
    Vec3f base, tip, width;
};
struct Fin {
    mmvr::SwingGate gate;
    mmvr::ContactWindow window;
    mmvr::FixedHistory<Sample> samples;
    ColliderQuad collider{};
    bool visible = false;
    double lastHit = -100;
};
Fin fins[2];
Player* owner = nullptr;
int scene = -1;
uint64_t epoch = 0, originEpoch = 0;
double now = 0;
std::chrono::steady_clock::time_point recorded;
Vec3f Offset(Vec3f a, Vec3f w, float sign) {
    return { a.x + sign * w.x, a.y + sign * w.y, a.z + sign * w.z };
}
bool Eligible(PlayState* play, Player* p) {
    return mmvrgame::PhysicalFinMode(p) && mmvrgame::FormTrackingReady(p) && !MMVR_FormAimStage(p) &&
           !(p->stateFlags2 & PLAYER_STATE2_USING_OCARINA) && !p->heldActor &&
           p->leftHandType != PLAYER_MODELTYPE_LH_BOTTLE;
}
bool Wall(PlayState* play, Player* p, Vec3f a, Vec3f b, Vec3f& hit) {
    CollisionPoly* poly = nullptr;
    int bg = BGCHECK_SCENE;
    return BgCheck_EntityLineTest2(&play->colCtx, &a, &b, &hit, &poly, true, true, true, true, &bg, &p->actor);
}
} // namespace
namespace mmvrgame {
bool PhysicalFinMode(Player* p) {
    return p && p->transformation == PLAYER_FORM_ZORA && mmvr::FirstPersonRequested() &&
           mmvr::GetSettings().Get(mmvr::Setting::PhysicalFins) > .5f;
}
void ClearFinCombat() {
    for (auto& f : fins)
        f = Fin{};
    owner = nullptr;
}
Collider* FinDebugCollider(int hand) {
    return hand >= 0 && hand < 2 && fins[hand].visible ? &fins[hand].collider.base : nullptr;
}
void UpdateFinCombat(const mmvr::TrackingFrame& frame, const mmvr::Matrix& view, const mmvr::Matrix& head,
                     const mmvr::Matrix* models) {
    auto* play = gPlayState;
    auto* p = play ? GET_PLAYER(play) : nullptr;
    if (!play || !Eligible(play, p)) {
        ClearFinCombat();
        return;
    }
    if (owner != p || scene != play->sceneId || epoch != frame.epoch || originEpoch != frame.originEpoch) {
        ClearFinCombat();
        owner = p;
        scene = play->sceneId;
        epoch = frame.epoch;
        originEpoch = frame.originEpoch;
    }
    now = frame.timeSeconds;
    recorded = std::chrono::steady_clock::now();
    const auto& s = mmvr::GetSettings();
    mmvr::SwingTuning tune{ s.Get(mmvr::Setting::SwingSpeed), s.Get(mmvr::Setting::SwingDistance),
                            s.Get(mmvr::Setting::SwingResetSpeed), s.Get(mmvr::Setting::SwingCooldown) };
    auto invView = mmvr::InversePose(view), worldHead = FormHeadPose();
    for (int h = 0; h < 2; ++h) {
        auto& f = fins[h];
        const auto& model = models[h];
        if ((h == 1 - mmvr::SwordController(s) && ShieldRaised()) || !model.m[3][3] || !frame.handValid[h] ||
            !frame.handTracked[h]) {
            f = Fin{};
            continue;
        }
        auto grip = mmvr::Multiply(mmvr::PoseMatrix(frame.hands[h]), mmvr::InversePose(mmvr::PoseMatrix(frame.origin)));
        auto gripWorld = mmvr::Multiply(grip, view);
        Vec3f base{ model.m[3][0], model.m[3][1], model.m[3][2] }, axis{ model.m[0][0], model.m[0][1], model.m[0][2] },
            width{ model.m[1][0], model.m[1][1], model.m[1][2] };
        float length = std::sqrt(SQ(axis.x) + SQ(axis.y) + SQ(axis.z)),
              wlen = std::sqrt(SQ(width.x) + SQ(width.y) + SQ(width.z));
        if (!std::isfinite(length) || length < .00001f || !std::isfinite(wlen) || wlen < .00001f) {
            f = Fin{};
            continue;
        }
        // Native fin length is X. Choose its forward end using the actual grip pose,
        // independent of the left/right resource's authored sign.
        float sign = axis.x * (-gripWorld.m[2][0]) + axis.y * (-gripWorld.m[2][1]) + axis.z * (-gripWorld.m[2][2]) >= 0
                         ? 1.f
                         : -1.f;
        float reach = s.Get(mmvr::Setting::FinReach) * mmvr::WorldUnitsPerMetre();
        Vec3f tip{ base.x + axis.x / length * reach * sign, base.y + axis.y / length * reach * sign,
                   base.z + axis.z / length * reach * sign };
        width = { width.x / wlen * 2, width.y / wlen * 2, width.z / wlen * 2 };
        const Vec3f unboundedTip = tip;
        Vec3f hit, eye{ worldHead.m[3][0], worldHead.m[3][1], worldHead.m[3][2] };
        float distance = std::sqrt(SQ(base.x - eye.x) + SQ(base.y - eye.y) + SQ(base.z - eye.z));
        bool blocked =
            !std::isfinite(distance) || distance > s.Get(mmvr::Setting::AimReach) * mmvr::WorldUnitsPerMetre() || Wall(play, p, eye, base, hit);
        if (!blocked && Wall(play, p, base, tip, hit))
            tip = { hit.x + (base.x - hit.x) * .01f, hit.y + (base.y - hit.y) * .01f, hit.z + (base.z - hit.z) * .01f };
        if (!f.samples.empty() &&
            (Wall(play, p, f.samples.back().base, base, hit) || Wall(play, p, f.samples.back().tip, tip, hit)))
            blocked = true;
        if (blocked) {
            f = Fin{};
            continue;
        }
        auto local = mmvr::Multiply(mmvr::YawPose(0, unboundedTip.x, unboundedTip.y, unboundedTip.z), invView);
        const float worldUnits = mmvr::WorldUnitsPerMetre();
        mmvr::MotionPoint raw{ now, local.m[3][0] / worldUnits + head.m[3][0], local.m[3][1] / worldUnits,
                               local.m[3][2] / worldUnits + head.m[3][2] };
        if (!f.samples.empty() &&
            (now < f.samples.back().time || now - f.samples.back().time > .15 ||
             std::hypot(base.x - f.samples.back().base.x, base.z - f.samples.back().base.z) > 20)) {
            f = Fin{};
        }
        if (f.gate.Update(raw, frame.epoch, true, tune))
            f.window.Arm(now, s.Get(mmvr::Setting::SwordWindow));
        if (!f.samples.empty() && f.samples.back().time == now)
            f.samples.pop_back();
        f.samples.push_back({ now, base, tip, width });
        while (f.samples.size() > 32 || now - f.samples.front().time > .18)
            f.samples.pop_front();
        f.collider = p->meleeWeaponQuads[h];
        f.collider.base.actor = &p->actor;
        f.collider.base.atFlags = AT_ON | AT_TYPE_PLAYER;
        f.collider.elem.atDmgInfo.dmgFlags = DMG_ZORA_PUNCH;
        f.collider.elem.atDmgInfo.damage = 1;
        f.collider.elem.atDmgInfo.effect = 0;
        f.collider.elem.atElemFlags = ATELEM_ON | ATELEM_SFX_NORMAL;
        Vec3f a = Offset(base, width, -1), b = Offset(tip, width, -1), c = Offset(base, width, 1),
              d = Offset(tip, width, 1);
        Collider_SetQuadVertices(&f.collider, &a, &b, &c, &d);
        f.visible = true;
    }
}
void ResolveFinCombat(PlayState* play) {
    auto* p = GET_PLAYER(play);
    if (p != owner || scene != play->sceneId || !Eligible(play, p) ||
        std::chrono::steady_clock::now() - recorded > std::chrono::milliseconds(150)) {
        ClearFinCombat();
        return;
    }
    for (int h = 0; h < 2; ++h) {
        auto& f = fins[h];
        if (!f.visible || !f.window.Active(now) ||
            now - f.lastHit < mmvr::GetSettings().Get(mmvr::Setting::SwingCooldown))
            continue;
        auto resolve = [&](Vec3f a, Vec3f b, Vec3f c, Vec3f d) {
            Collider_ResetQuadAT(play, &f.collider.base);
            Collider_SetQuadVertices(&f.collider, &a, &b, &c, &d);
            CollisionCheck_AC(play, &play->colChkCtx, &f.collider.base);
            return (f.collider.base.atFlags & (AT_HIT | AT_BOUNCED)) != 0;
        };
        bool hit = false;
        for (size_t i = 1; i < f.samples.size() && !hit; ++i) {
            auto& a = f.samples[i - 1];
            auto& b = f.samples[i];
            hit = resolve(b.base, b.tip, a.base, a.tip) ||
                  resolve(Offset(b.base, b.width, -1), Offset(b.tip, b.width, -1), Offset(b.base, b.width, 1),
                          Offset(b.tip, b.width, 1));
        }
        if (hit) {
            f.window.Contact();
            f.lastHit = now;
            mmvr::HapticPulse(h, .5f);
            if (mmvr::GetSettings().Get(mmvr::Setting::SwordDiagnostics) > .5f)
                std::ofstream("mmvr-combat.log", std::ios::app)
                    << "fin-contact hand=" << h << " target=" << (f.collider.base.at ? f.collider.base.at->id : -1)
                    << " reach=" << mmvr::GetSettings().Get(mmvr::Setting::FinReach) << " t=" << now << "\n";
        }
        if (!f.samples.empty()) {
            auto last = f.samples.back();
            f.samples.clear();
            f.samples.push_back(last);
        }
    }
}
} // namespace mmvrgame
#endif

#if defined(MMVR_ENABLE) && defined(MMVR_STATE_NATIVE_BACKEND)
#include "NativeStateFields.h"
extern "C" void MMVR_VisitVrFinCollisionState(MMVR_StateSink* sink) {
    mmvrgame::NativeStateField(sink,"vr/fins/fins",fins);
    mmvrgame::NativeStateField(sink,"vr/fins/owner",owner);
    mmvrgame::NativeStateField(sink,"vr/fins/scene",scene);
    mmvrgame::NativeStateField(sink,"vr/fins/epoch",epoch);
    mmvrgame::NativeStateField(sink,"vr/fins/originEpoch",originEpoch);
    mmvrgame::NativeStateField(sink,"vr/fins/now",now);
    mmvrgame::NativeStateField(sink,"vr/fins/recorded",recorded);
    for(auto& value:fins)MMVR_StateVisitColliderQuad(sink,&value.collider);
}
#endif

#if defined(MMVR_ENABLE) && defined(MMVR_STATE_NATIVE_BACKEND)
#include "NativeTrackingResume.h"
namespace mmvrgame {
void RebaseFinTracking(const mmvr::TrackingFrame& f) {
    for(auto& fin:fins) {
        fin.gate.Rebase(now,f.timeSeconds,f.epoch);
        fin.window.Rebase(now,f.timeSeconds);
        fin.lastHit=f.timeSeconds+(fin.lastHit-now);
        fin.samples.clear();fin.visible=false;
    }
    now=f.timeSeconds;epoch=f.epoch;originEpoch=f.originEpoch;recorded={};
    owner=gPlayState?GET_PLAYER(gPlayState):nullptr;scene=gPlayState?gPlayState->sceneId:-1;
}
}
#endif
