#ifdef MMVR_ENABLE
#include "GoronCombat.h"
#include "ItemUse.h"
#include "FormAim.h"
#include "combat.h"
#include "runtime.h"
#include "ui.h"
#include <chrono>
#include <fstream>
#include <array>
extern "C" {
#include "global.h"
void CollisionCheck_AC(PlayState*, CollisionCheckContext*, Collider*);
}
namespace {
Player* owner = nullptr;
int scene = -1;
bool equipped = false;
struct Fist {
    mmvr::SwingGate gate;
    mmvr::ContactWindow window;
    ColliderQuad quad{};
    mmvr::MotionPoint local{};
    Vec3f position{}, previous{};
    double lastHit = -100, fireUntil = 0;
    bool valid = false, punchSpent = false;
    float forwardSpeed = 0;
    struct Sample {
        Vec3f base, tip;
    };
    std::array<Sample, 33> path{};
    size_t pathCount = 0;
    Vec3f tip{};
};
Fist fists[2];
double now = 0;
uint64_t epoch = 0, originEpoch = 0;
float yaw = 0;
#ifdef MMVR_LOCAL_TEST_TOOLS
bool legacyPunchRays = false;
unsigned punchRayCount = 0;
#endif
std::chrono::steady_clock::time_point recorded;
void Clear() {
    for (auto& f : fists)
        f = Fist{};
}
bool Eligible(Player* p) {
    return mmvrgame::GoronFists(p) && mmvrgame::FormTrackingReady(p) && !p->heldActor &&
           !(p->stateFlags3 & PLAYER_STATE3_1000) &&
           !(p->stateFlags1 & (PLAYER_STATE1_400000 | PLAYER_STATE1_8000000)) &&
           !(p->stateFlags2 & PLAYER_STATE2_USING_OCARINA) && mmvr::HeldMaskItem() < 0;
}
bool Wall(PlayState* play, Player* p, Vec3f a, Vec3f b) {
    CollisionPoly* poly = nullptr;
    int bg = BGCHECK_SCENE;
    Vec3f hit;
    return BgCheck_EntityLineTest2(&play->colCtx, &a, &b, &hit, &poly, true, true, true, true, &bg, &p->actor);
}
void Face(ColliderQuad& q, Vec3f center, int normal, float radius) {
    Vec3f v[4];
    int a = (normal + 1) % 3, b = (normal + 2) % 3;
    for (int i = 0; i < 4; ++i) {
        v[i] = center;
        (&v[i].x)[a] += (i & 1) ? radius : -radius;
        (&v[i].x)[b] += (i & 2) ? radius : -radius;
    }
    Collider_SetQuadVertices(&q, &v[0], &v[1], &v[2], &v[3]);
}
} // namespace
namespace mmvrgame {
bool GoronFists(Player* p) {
    return p && gPlayState && p == owner && scene == gPlayState->sceneId && p->transformation == PLAYER_FORM_GORON &&
           equipped && mmvr::FirstPersonRequested() && mmvr::GetSettings().Get(mmvr::Setting::PhysicalFists) > .5f &&
           !HasItemInHand(gPlayState);
}
void ProcessGoronInput(PlayState* play) {
    auto* p = GET_PLAYER(play);
    if (p != owner || scene != play->sceneId || p->transformation != PLAYER_FORM_GORON) {
        owner = p;
        scene = play->sceneId;
        equipped = false;
        Clear();
    }
    // Equipping is an input action; a stale render pose must not discard B.
    // Tracking freshness remains mandatory in Eligible() before any punch can hit.
    if (p->transformation != PLAYER_FORM_GORON || !mmvr::FirstPersonRequested() ||
        mmvr::GetSettings().Get(mmvr::Setting::PhysicalFists) < .5f || !mmvr::PhysicalActionsAllowed() ||
        p->csAction != PLAYER_CSACTION_NONE || play->csCtx.state != CS_STATE_IDLE ||
        play->transitionTrigger != TRANS_TRIGGER_OFF || play->pauseCtx.state != PAUSE_STATE_OFF ||
        play->msgCtx.msgMode != MSGMODE_NONE || gSaveContext.save.saveInfo.playerData.health <= 0)
        return;
    // Rolling B owns the native ball jump; standing B owns physical fists.
    if ((p->stateFlags3 & PLAYER_STATE3_1000) || (p->stateFlags1 & PLAYER_STATE1_400000)) return;
    auto& input = *CONTROLLER1(&play->state);
    if (input.press.button & BTN_B) {
        if (HasItemInHand(play)) {
            StowItem(play);
            equipped = false;
            Clear();
            input.cur.button &= ~BTN_B;
            input.press.button &= ~BTN_B;
        } else {
            equipped = !equipped;
            Clear();
            mmvr::HapticPulse(mmvr::SwordController(mmvr::GetSettings()), .2f);
        }
    }
    // Do not let the same held/pressed B start native item/punch animation after
    // toggling the tracked mesh. Subsequent presses must remain immediate.
    input.cur.button &= ~BTN_B;
    input.press.button &= ~BTN_B;
}
void UpdateGoronCombat(const mmvr::TrackingFrame& frame, const mmvr::Matrix& view, const mmvr::Matrix& head) {
    auto* play = gPlayState;
    auto* p = play ? GET_PLAYER(play) : nullptr;
    if (!Eligible(p)) {
        Clear();
        return;
    }
    if (epoch != frame.epoch || originEpoch != frame.originEpoch || std::abs(frame.snapYaw - yaw) > .001f) {
        Clear();
        epoch = frame.epoch;
        originEpoch = frame.originEpoch;
        yaw = frame.snapYaw;
    }
    now = frame.timeSeconds;
    recorded = std::chrono::steady_clock::now();
    auto inverse = mmvr::InversePose(mmvr::PoseMatrix(frame.origin));
    auto eye = FormHeadPose();
    const auto& s = mmvr::GetSettings();
    mmvr::SwingTuning tune{ s.Get(mmvr::Setting::PunchSpeed), s.Get(mmvr::Setting::PunchDistance),
                            s.Get(mmvr::Setting::SwingResetSpeed), s.Get(mmvr::Setting::SwingCooldown) };
    for (int h = 0; h < 2; ++h) {
        auto& f = fists[h];
        if (!frame.handTracked[h] || !frame.handValid[h] || !frame.aimValid[h]) {
            f = Fist{};
            continue;
        }
        auto local = mmvr::Multiply(mmvr::PoseMatrix(frame.hands[h]), inverse);
        auto aim = mmvr::Multiply(mmvr::PoseMatrix(frame.aims[h]), inverse);
        mmvr::MotionPoint point{ now, local.m[3][0], local.m[3][1], local.m[3][2] };
        const float units = mmvr::WorldUnitsPerMetre();
        local.m[3][0] = (point.x - head.m[3][0]) * units;
        local.m[3][1] = point.y * units;
        local.m[3][2] = (point.z - head.m[3][2]) * units;
        auto world = mmvr::Multiply(local, view);
        Vec3f current{ world.m[3][0], world.m[3][1], world.m[3][2] }, from{ eye.m[3][0], eye.m[3][1], eye.m[3][2] };
        if (f.valid && now == f.local.time)
            continue;
        double dt = now - f.local.time;
        float dx = point.x - f.local.x, dy = point.y - f.local.y, dz = point.z - f.local.z;
        if (!std::isfinite(now) || (f.valid && (dt <= 0 || dt > .15 || dx * dx + dy * dy + dz * dz > .25f * .25f)) ||
            std::sqrt(SQ(current.x - from.x) + SQ(current.y - from.y) + SQ(current.z - from.z)) >
                s.Get(mmvr::Setting::AimReach) * units ||
            Wall(play, p, from, current) || (f.valid && Wall(play, p, f.position, current))) {
            f = Fist{};
            continue;
        }
        float forward = f.valid ? float((-aim.m[2][0] * dx - aim.m[2][1] * dy - aim.m[2][2] * dz) / dt) : 0;
        // One outward punch, one contact window. Retraction or rest rearms the
        // stroke; simply pushing the fist deeper cannot deal a second hit.
        if (forward < std::min(.2f, s.Get(mmvr::Setting::SwingResetSpeed)))
            f.punchSpent = false;
        if (f.gate.Update(point, frame.epoch, true, tune) && !f.punchSpent &&
            forward >= s.Get(mmvr::Setting::PunchSpeed) * .6f) {
            f.punchSpent = true;
            f.window.Arm(now, s.Get(mmvr::Setting::SwordWindow));
            f.fireUntil = now + .22;
            if (s.Get(mmvr::Setting::SwordDiagnostics) > .5f)
                std::ofstream("mmvr-combat.log", std::ios::app)
                    << "goron-punch hand=" << h << " speed=" << forward << " t=" << now << "\n";
        }
        auto worldAim = mmvr::Multiply(aim, view);
        float extension = s.Get(mmvr::Setting::PunchExtension) * units;
        Vec3f tip{ current.x - worldAim.m[2][0] * extension, current.y - worldAim.m[2][1] * extension,
                   current.z - worldAim.m[2][2] * extension };
        if (Wall(play, p, current, tip))
            tip = current;
        if (f.pathCount >= f.path.size()) {
            f = Fist{};
            continue;
        }
        if (!f.pathCount)
            f.path[f.pathCount++] = { f.valid ? f.position : current, f.valid ? f.tip : tip };
        f.path[f.pathCount++] = { current, tip };
        f.tip = tip;
        f.forwardSpeed = forward;
        f.previous = f.valid ? f.position : current;
        f.position = current;
        f.local = point;
        f.valid = true;
        f.quad = p->meleeWeaponQuads[h];
        f.quad.base.actor = &p->actor;
        f.quad.base.atFlags = AT_ON | AT_TYPE_PLAYER;
        f.quad.elem.atDmgInfo.dmgFlags = DMG_GORON_PUNCH;
        f.quad.elem.atDmgInfo.damage = 2;
        f.quad.elem.atDmgInfo.effect = 0;
        f.quad.elem.atElemFlags = ATELEM_ON | ATELEM_SFX_NORMAL;
        // Longitudinal debug outline includes the added forward reach.
        float r = s.Get(mmvr::Setting::PunchRadius) * units;
        Vec3f a{}, b{}, c{}, d{};
        for (int k = 0; k < 3; ++k) {
            float side = worldAim.m[0][k] * r, back = worldAim.m[2][k] * r;
            (&a.x)[k] = (&current.x)[k] + back - side;
            (&b.x)[k] = (&current.x)[k] + back + side;
            (&c.x)[k] = (&tip.x)[k] - back - side;
            (&d.x)[k] = (&tip.x)[k] - back + side;
        }
        Collider_SetQuadVertices(&f.quad, &a, &b, &c, &d);
    }
}
Collider* GoronDebugCollider(int hand) {
    return hand >= 0 && hand < 2 && fists[hand].valid ? &fists[hand].quad.base : nullptr;
}
float GoronPunchFire(int hand) {
    return hand >= 0 && hand < 2 && fists[hand].valid ? std::clamp(float((fists[hand].fireUntil - now) / .22), 0.f, 1.f)
                                                      : 0;
}
#ifdef MMVR_LOCAL_TEST_TOOLS
void SetGoronRayReview(bool legacy) { legacyPunchRays = legacy; punchRayCount = 0; }
unsigned GoronRayReviewCount() { return punchRayCount; }
#endif
void ResolveGoronCombat(PlayState* play) {
    auto* p = GET_PLAYER(play);
    if (!Eligible(p) || std::chrono::steady_clock::now() - recorded > std::chrono::milliseconds(150)) {
        Clear();
        return;
    }
    for (int h = 0; h < 2; ++h) {
        auto& f = fists[h];
        if (!f.valid || !f.window.Active(now) ||
            f.forwardSpeed < mmvr::GetSettings().Get(mmvr::Setting::PunchSpeed) * .5f ||
            now - f.lastHit < mmvr::GetSettings().Get(mmvr::Setting::SwingCooldown)) {
            f.pathCount = 0;
            continue;
        }
        bool hit = false;
        float radius = mmvr::GetSettings().Get(mmvr::Setting::PunchRadius) * mmvr::WorldUnitsPerMetre();
        // Sweep the fist volume between tracked samples, with native enemy damage tables.
        for (size_t segment = 1; segment < f.pathCount && !hit; ++segment) {
            auto before = f.path[segment - 1], after = f.path[segment];
            for (int station = 0; station <= 2 && !hit; ++station) {
                float along = station * .5f;
                Vec3f a{ before.base.x + (before.tip.x - before.base.x) * along,
                         before.base.y + (before.tip.y - before.base.y) * along,
                         before.base.z + (before.tip.z - before.base.z) * along };
                Vec3f b{ after.base.x + (after.tip.x - after.base.x) * along,
                         after.base.y + (after.tip.y - after.base.y) * along,
                         after.base.z + (after.tip.z - after.base.z) * along };
                float travel = std::sqrt(SQ(b.x - a.x) + SQ(b.y - a.y) + SQ(b.z - a.z));
                int steps = std::clamp(int(std::ceil(travel / std::max(radius * .5f, 1.f))), 1, 16);
                for (int step = 0; step <= steps && !hit; ++step) {
                    float t = float(step) / steps;
                    Vec3f center{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
                    const auto headPose = FormHeadPose();
                    Vec3f eye{ headPose.m[3][0], headPose.m[3][1], headPose.m[3][2] };
                    // All three faces share this point. Native line tests reset
                    // their own scratch table; collision AC does not move scenery.
                    // Resolve visibility once, then retain the original face order.
                    auto blocked = [&] {
#ifdef MMVR_LOCAL_TEST_TOOLS
                        ++punchRayCount;
#endif
                        return Wall(play, p, eye, center);
                    };
#ifdef MMVR_LOCAL_TEST_TOOLS
                    if (!legacyPunchRays && blocked()) continue;
#else
                    if (blocked()) continue;
#endif
                    for (int axis = 0; axis < 3 && !hit; ++axis) {
#ifdef MMVR_LOCAL_TEST_TOOLS
                        if (legacyPunchRays && blocked()) continue;
#endif
                        Collider_ResetQuadAT(play, &f.quad.base);
                        Face(f.quad, center, axis, radius);
                        CollisionCheck_AC(play, &play->colChkCtx, &f.quad.base);
                        hit = (f.quad.base.atFlags & (AT_HIT | AT_BOUNCED)) != 0;
                    }
                }
            }
        }
        f.pathCount = 0;
        if (hit) {
            f.window.Contact();
            f.lastHit = now;
            mmvr::HapticPulse(h, .65f);
            if (mmvr::GetSettings().Get(mmvr::Setting::SwordDiagnostics) > .5f)
                std::ofstream("mmvr-combat.log", std::ios::app)
                    << "goron-contact hand=" << h << " target=" << (f.quad.base.at ? f.quad.base.at->id : -1)
                    << " t=" << now << "\n";
        }
        f.previous = f.position;
    }
}
} // namespace mmvrgame
#endif

#if defined(MMVR_ENABLE) && defined(MMVR_STATE_NATIVE_BACKEND)
#include "NativeStateFields.h"
extern "C" void MMVR_VisitVrGoronCollisionState(MMVR_StateSink* sink) {
    mmvrgame::NativeStateField(sink,"vr/goron/owner",owner);
    mmvrgame::NativeStateField(sink,"vr/goron/scene",scene);
    mmvrgame::NativeStateField(sink,"vr/goron/equipped",equipped);
    mmvrgame::NativeStateField(sink,"vr/goron/fists",fists);
    mmvrgame::NativeStateField(sink,"vr/goron/now",now);
    mmvrgame::NativeStateField(sink,"vr/goron/epoch",epoch);
    mmvrgame::NativeStateField(sink,"vr/goron/originEpoch",originEpoch);
    mmvrgame::NativeStateField(sink,"vr/goron/yaw",yaw);
    mmvrgame::NativeStateField(sink,"vr/goron/recorded",recorded);
    for(auto& value:fists)MMVR_StateVisitColliderQuad(sink,&value.quad);
}
#endif

#if defined(MMVR_ENABLE) && defined(MMVR_STATE_NATIVE_BACKEND)
#include "NativeTrackingResume.h"
namespace mmvrgame {
void RebaseGoronTracking(const mmvr::TrackingFrame& f) {
    for(auto& fist:fists) {
        fist.gate.Rebase(now,f.timeSeconds,f.epoch);
        fist.window.Rebase(now,f.timeSeconds);
        fist.lastHit=f.timeSeconds+(fist.lastHit-now);
        fist.fireUntil=f.timeSeconds+(fist.fireUntil-now);
        fist.pathCount=0;fist.valid=false;fist.forwardSpeed=0;
    }
    now=f.timeSeconds;epoch=f.epoch;originEpoch=f.originEpoch;yaw=f.snapYaw;recorded={};
    owner=gPlayState?GET_PLAYER(gPlayState):nullptr;scene=gPlayState?gPlayState->sceneId:-1;
    // equipped is the saved player choice, not a tracking-history bit.
}
}
#endif
