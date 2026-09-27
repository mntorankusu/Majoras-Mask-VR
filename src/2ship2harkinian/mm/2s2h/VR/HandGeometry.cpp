#ifdef MMVR_ENABLE
#include "HandGeometry.h"
#include "Carry.h"
#include "hand_collision.h"
#include <memory>
#include "Camera.h"
#include "FormAim.h"
#include "FinCombat.h"
#include "GoronCombat.h"
#include "Interactions.h"
extern "C" {
#include "global.h"
}
namespace {
mmvr::HandCollision hands[2];
double lastTime=0;
mmvr::Matrix lastView{};
// Use the game's spatial index and transformed dynamic polygons, but keep
// sphere/triangle contacts in floating point. Query every cell touched by the
// sphere so a palm does not slip through a partition boundary.
bool WorldContact(CollisionContext& context, Player* player, mmvr::HandPoint point, float radius,
                  mmvr::HandContact& hit) {
    auto list=[&](SSList head,const SSNode* nodes,size_t nodeCount,const CollisionPoly* polygons,
                  size_t polygonCount,const Vec3s* vertices,size_t vertexCount) {
        if(!nodes||!polygons||!vertices)return false;
        for(size_t walked=0;head.head!=SS_NULL&&head.head<nodeCount&&walked<nodeCount;++walked) {
            auto node=nodes[head.head];head.head=node.next;
            if(node.polyId<0||size_t(node.polyId)>=polygonCount)continue;
            const auto& poly=polygons[node.polyId];
            if(COLPOLY_VIA_FLAG_TEST(poly.flags_vIA,COLPOLY_IGNORE_ENTITY))continue;
            unsigned indices[3]={COLPOLY_VTX_INDEX(poly.flags_vIA),COLPOLY_VTX_INDEX(poly.flags_vIB),poly.vIC};
            if(indices[0]>=vertexCount||indices[1]>=vertexCount||indices[2]>=vertexCount)continue;
            mmvr::HandPoint triangle[3];
            for(int i=0;i<3;++i) {
                auto v=vertices[indices[i]];
                triangle[i]={float(v.x),float(v.y),float(v.z)};
            }
            if(mmvr::HandTriangleContact(point,radius,triangle[0],triangle[1],triangle[2],hit))return true;
        }
        return false;
    };
    if(context.colHeader&&context.lookupTbl&&context.subdivAmount.x>0&&context.subdivAmount.y>0&&context.subdivAmount.z>0) {
        int low[3],high[3];
        const float minimum[3]={context.minBounds.x,context.minBounds.y,context.minBounds.z};
        const float inverse[3]={context.subdivLengthInv.x,context.subdivLengthInv.y,context.subdivLengthInv.z};
        const int count[3]={context.subdivAmount.x,context.subdivAmount.y,context.subdivAmount.z};
        for(int i=0;i<3;++i) {
            low[i]=std::clamp(int((point[i]-radius-minimum[i])*inverse[i]),0,count[i]-1);
            high[i]=std::clamp(int((point[i]+radius-minimum[i])*inverse[i]),0,count[i]-1);
        }
        const auto& header=*context.colHeader;
        for(int z=low[2];z<=high[2];++z)for(int y=low[1];y<=high[1];++y)for(int x=low[0];x<=high[0];++x) {
            auto& lookup=context.lookupTbl[(z*count[1]+y)*count[0]+x];
            for(auto head:{lookup.floor,lookup.wall,lookup.ceiling})
                if(list(head,context.polyNodes.tbl,context.polyNodes.count,header.polyList,header.numPolygons,
                        header.vtxList,header.numVertices))return true;
        }
    }
    auto& dyna=context.dyna;
    for(int i=0;i<BG_ACTOR_MAX;++i) {
        auto flags=dyna.bgActorFlags[i];auto& actor=dyna.bgActors[i];
        if(!(flags&BGACTOR_IN_USE)||(flags&(BGACTOR_1|BGACTOR_COLLISION_DISABLED))||
           actor.actor==&player->actor||actor.actor==player->rideActor||
           (player->heldActor&&actor.actor==player->heldActor))continue;
        mmvr::HandPoint center{float(actor.boundingSphere.center.x),float(actor.boundingSphere.center.y),float(actor.boundingSphere.center.z)};
        if(mmvr::HandLength(mmvr::HandSub(point,center))>radius+actor.boundingSphere.radius)continue;
        const SSList lists[3]={actor.dynaLookup.floor,actor.dynaLookup.wall,actor.dynaLookup.ceiling};
        for(int type=0;type<3;++type) {
            if((type==0&&(flags&BGACTOR_FLOOR_COLLISION_DISABLED))||
               (type==2&&(flags&BGACTOR_CEILING_COLLISION_DISABLED)))continue;
            if(list(lists[type],dyna.polyNodes.tbl,dyna.polyNodes.count,dyna.polyList,dyna.polyListMax,
                    dyna.vtxList,dyna.vtxListMax))return true;
        }
    }
    return false;
}
bool PropContact(PlayState* play,Player* player,mmvr::HandPoint point,float radius,mmvr::HandContact& hit) {
    auto pointOf=[](const auto& v){return mmvr::HandPoint{float(v.x),float(v.y),float(v.z)};};
    auto sphere=[&](const Sphere16& s){return mmvr::HandSphereContact(point,radius,pointOf(s.center),s.radius,hit);};
    auto triangle=[&](const Vec3f* v){return mmvr::HandTriangleContact(point,radius,pointOf(v[0]),pointOf(v[1]),pointOf(v[2]),hit);};
    auto& ctx=play->colChkCtx;
    for(int i=0;i<std::clamp(ctx.colOCCount,0,int(ARRAY_COUNT(ctx.colOC)));++i) {
        auto* col=ctx.colOC[i];auto* actor=col?col->actor:nullptr;
        if(!actor||!actor->update||actor==&player->actor||actor==player->heldActor||actor==player->rideActor||actor->parent==&player->actor||!(col->ocFlags1&OC1_ON))continue;
        // The current mount is part of the player's locomotion frame. Epona's
        // animated body cylinders must not shove the rider's tracked palms away
        // each native tick. Other actors and world geometry still block hands.
        // NPC/enemy cylinders are interaction bounds, not exact model surfaces.
        // Keep them pass-through rather than inventing an invisible solid body.
        if(actor->category!=ACTORCAT_PROP&&actor->category!=ACTORCAT_BG&&actor->category!=ACTORCAT_DOOR&&actor->category!=ACTORCAT_EXPLOSIVES&&actor->id!=ACTOR_EN_MM)continue;
        if(actor->id==ACTOR_EN_KUSA||actor->id==ACTOR_EN_KUSA2||actor->id==ACTOR_OBJ_GRASS||
           actor->id==ACTOR_OBJ_GRASS_CARRY||actor->id==ACTOR_OBJ_GRASS_UNIT)continue;
        if(col->shape==COLSHAPE_CYLINDER && !(reinterpret_cast<ColliderCylinder*>(col)->elem.ocElemFlags&OCELEM_ON))continue;
        Vec3f surface;bool inside=false;
        if(mmvrgame::CarryPropSurface(actor,{point[0],point[1],point[2]},surface,inside,radius)) {
            auto delta=mmvr::HandSub(point,pointOf(surface));float distance=mmvr::HandLength(delta);
            if(inside || distance<radius) {
                hit.normal=distance>.0001f?mmvr::HandScale(delta,(inside?-1.f:1.f)/distance):mmvr::HandPoint{0,1,0};
                hit.depth=inside?radius+distance:radius-distance;
                return true;
            }
            continue; // The broad native body cylinder must not override the mesh surface.
        }
        switch(col->shape) {
            case COLSHAPE_CYLINDER: {
                auto& c=*reinterpret_cast<ColliderCylinder*>(col);
                if(!(c.elem.ocElemFlags&OCELEM_ON))break;
                auto base=pointOf(c.dim.pos);base[1]+=c.dim.yShift;
                if(mmvr::HandCylinderContact(point,radius,base,c.dim.radius,c.dim.height,hit))return true;
                break;
            }
            case COLSHAPE_SPHERE: {
                auto& c=*reinterpret_cast<ColliderSphere*>(col);
                if((c.elem.ocElemFlags&OCELEM_ON)&&sphere(c.dim.worldSphere))return true;
                break;
            }
            case COLSHAPE_JNTSPH: {
                auto& c=*reinterpret_cast<ColliderJntSph*>(col);
                if(c.elements)for(int n=0;n<c.count;++n)
                    if((c.elements[n].base.ocElemFlags&OCELEM_ON)&&sphere(c.elements[n].dim.worldSphere))return true;
                break;
            }
            case COLSHAPE_TRIS: {
                auto& c=*reinterpret_cast<ColliderTris*>(col);
                if(c.elements)for(int n=0;n<c.count;++n)
                    if((c.elements[n].base.ocElemFlags&OCELEM_ON)&&triangle(c.elements[n].dim.vtx))return true;
                break;
            }
            default:break;
        }
    }
    return false;
}
mmvr::HandPoint WorldPosition(const XrPosef& pose, const mmvr::TrackingFrame& frame,
    const mmvr::Matrix& view, const mmvr::Matrix& head) {
    auto m=mmvr::Multiply(mmvr::PoseMatrix(pose),mmvr::InversePose(mmvr::PoseMatrix(frame.origin)));
    const float units=mmvr::WorldUnitsPerMetre();
    m.m[3][0]=(m.m[3][0]-head.m[3][0])*units;
    m.m[3][1]*=units;
    m.m[3][2]=(m.m[3][2]-head.m[3][2])*units;
    m=mmvr::Multiply(m,view);
    return {m.m[3][0],m.m[3][1],m.m[3][2]};
}
}
namespace mmvrgame {
void ResetHandGeometry(){for(auto& hand:hands)hand.Reset();lastTime=0;lastView={};}
mmvr::TrackingFrame ResolveHandGeometry(PlayState* play, Player* player,
    const mmvr::TrackingFrame& filtered, const mmvr::TrackingFrame& raw,
    const mmvr::Matrix& view, const mmvr::Matrix& relativeHead) {
    auto output=filtered;
    const double dt=raw.timeSeconds-lastTime;
    lastTime=raw.timeSeconds;
    // Scene/recenter resets are explicit in Camera.cpp. Tracking discontinuities
    // and large scripted teleports must not drag old wall contacts into new space.
    if(dt<0||dt>.15)for(auto& hand:hands)hand.Reset();
    if(lastView.m[3][3]) {
        mmvr::HandPoint shift{view.m[3][0]-lastView.m[3][0],view.m[3][1]-lastView.m[3][1],view.m[3][2]-lastView.m[3][2]};
        if(mmvr::HandLength(shift)>80)for(auto& hand:hands)hand.Reset();
    }
    lastView=view;
    auto inverse=mmvr::InversePose(view);
    auto origin=mmvr::PoseMatrix(raw.origin);
    bool recovered=false;
    for(int h=0;h<2;++h) {
        if(!raw.handValid[h]||!raw.handTracked[h]) {hands[h].Reset();continue;}
        auto target=WorldPosition(filtered.hands[h],raw,view,relativeHead);
        auto controller=WorldPosition(raw.hands[h],raw,view,relativeHead);
        // Palm/finger envelope follows the actual calibrated model, not the
        // uncalibrated OpenXR grip origin. Both authored hands share this basis.
        auto model=mmvr::TrackedHandModel(filtered,view,relativeHead,h,h,mmvr::GetSettings());
        mmvr::HandPoint palm{};
        for(int k=0;k<3;++k)palm[k]=model.m[3][k]+275.f*model.m[1][k]-target[k];
        target=mmvr::HandAdd(target,palm);controller=mmvr::HandAdd(controller,palm);
        float radius=(player->transformation==PLAYER_FORM_GORON?6.f:3.5f)*mmvr::GetSettings().Get(mmvr::Setting::HandScale)*mmvr::ActiveWorldScale();
        auto query=[&](mmvr::HandPoint point,float r,mmvr::HandContact& hit) {
            return WorldContact(play->colCtx,player,point,r,hit)||PropContact(play,player,point,r,hit);
        };
        auto resolved=hands[h].Update(target,controller,radius,query);
        auto worldDelta=mmvr::HandSub(resolved,target);
        mmvr::HandPoint local{},xrDelta{};
        const float worldUnits=mmvr::WorldUnitsPerMetre();
        for(int c=0;c<3;++c)for(int r=0;r<3;++r)local[c]+=worldDelta[r]*inverse.m[r][c]/worldUnits;
        for(int c=0;c<3;++c)for(int r=0;r<3;++r)xrDelta[c]+=local[r]*origin.m[r][c];
        for(int c=0;c<3;++c) {
            (&output.hands[h].position.x)[c]+=xrDelta[c];
            (&output.aims[h].position.x)[c]+=xrDelta[c];
        }
        if(hands[h].blocked) {
            // Never use motion behind a wall as release velocity. The shared
            // interaction history computes the accepted movement instead.
            output.handVelocityValid[h]=false;
        }
        if(hands[h].recovered) {
            recovered=true;
            ClearTracking();ClearFinCombat();ClearFormTracking();
        }
    }
    if(recovered) output.epoch ^= uint64_t(1)<<63;
    return output;
}
}
#ifdef MMVR_LOCAL_TEST_TOOLS
extern "C" int MMVR_VerifyMountedHands(Player* player) {
    auto play=std::make_unique<PlayState>();
    Actor horse{};horse.id=ACTOR_EN_HORSE;horse.category=ACTORCAT_BG;
    horse.update=+[](Actor*,PlayState*){};
    ColliderCylinder cylinder{};cylinder.base.actor=&horse;cylinder.base.shape=COLSHAPE_CYLINDER;
    cylinder.base.ocFlags1=OC1_ON;cylinder.elem.ocElemFlags=OCELEM_ON;
    cylinder.dim.radius=50;cylinder.dim.height=100;cylinder.dim.pos={0,1000,0};
    play->colChkCtx.colOC[0]=&cylinder.base;play->colChkCtx.colOCCount=1;
    auto* oldMount=player->rideActor;player->rideActor=nullptr;
    mmvr::HandContact hit;
    bool okay=PropContact(play.get(),player,{0,1030,0},3.5f,hit);
    player->rideActor=&horse;
    okay&=!PropContact(play.get(),player,{0,1030,0},3.5f,hit);
    mmvrgame::ResetHandGeometry();
    mmvr::TrackingFrame frame{};frame.origin.orientation.w=frame.head.orientation.w=1;
    for(int hand=0;hand<2;++hand) {
        frame.handValid[hand]=frame.handTracked[hand]=frame.aimValid[hand]=true;
        frame.hands[hand].orientation.w=frame.aims[hand].orientation.w=1;
        frame.hands[hand].position={hand?.2f:-.2f,.3f,-.2f};
        frame.aims[hand]=frame.hands[hand];
    }
    const auto head=mmvr::YawPose(0);
    // Establish that the native horse volume actually displaces this fixture.
    // The mounted pass must then leave both grip and aim positions untouched.
    player->rideActor=nullptr;frame.timeSeconds=.5;
    auto blocked=mmvrgame::ResolveHandGeometry(play.get(),player,frame,frame,mmvr::YawPose(0,0,1020,0),head);
    float displacement=0;
    for(int hand=0;hand<2;++hand)for(int axis=0;axis<3;++axis)
        displacement+=std::abs((&blocked.hands[hand].position.x)[axis]-(&frame.hands[hand].position.x)[axis]);
    okay&=displacement>.01f;
    player->rideActor=&horse;mmvrgame::ResetHandGeometry();
    for(int sample=0;sample<180;++sample) {
        frame.timeSeconds=1.+sample/90.;
        const float x=sample*.8f,y=1020.f+std::sin(sample*.2f)*3.f;
        // Native mount colliders update at 20 Hz, while hands render at 90 Hz.
        cylinder.dim.pos.x=static_cast<s16>(int(sample*20/90)*3.6f);
        auto resolved=mmvrgame::ResolveHandGeometry(play.get(),player,frame,frame,
                                                   mmvr::YawPose(sample*.005f,x,y,0),head);
        for(int hand=0;hand<2;++hand)for(int axis=0;axis<3;++axis) {
            okay&=std::abs((&resolved.hands[hand].position.x)[axis]-(&frame.hands[hand].position.x)[axis])<.00001f;
            okay&=std::abs((&resolved.aims[hand].position.x)[axis]-(&frame.aims[hand].position.x)[axis])<.00001f;
        }
    }
    player->rideActor=oldMount;mmvrgame::ResetHandGeometry();
    return okay;
}
extern "C" int MMVR_VerifyHandWorldContacts(Player* player) {
    CollisionContext context{};
    Vec3s vertices[]={{10,-10,-10},{10,10,-10},{10,0,10}};
    CollisionPoly poly{};poly.flags_vIA=0;poly.flags_vIB=1;poly.vIC=2;
    CollisionHeader header{};header.numVertices=3;header.vtxList=vertices;header.numPolygons=1;header.polyList=&poly;
    StaticLookup lookup[2]{};
    for(auto& cell:lookup)cell.floor.head=cell.wall.head=cell.ceiling.head=SS_NULL;
    lookup[1].wall.head=0;
    SSNode node{0,SS_NULL};
    context.colHeader=&header;context.lookupTbl=lookup;context.polyNodes.tbl=&node;context.polyNodes.count=1;
    context.subdivAmount={2,1,1};context.subdivLengthInv={.1f,.1f,.1f};
    mmvr::HandContact hit;
    bool okay=WorldContact(context,player,{9.5f,0,0},1,hit)&&std::abs(hit.depth-.5f)<.0001f&&hit.normal[0]<0;
    okay&=WorldContact(context,player,{10.5f,0,0},1,hit)&&hit.normal[0]>0;
    okay&=!WorldContact(context,player,{8.5f,0,0},1,hit);
    lookup[1].wall.head=SS_NULL;
    Actor moving{};
    auto& dynamic=context.dyna;
    dynamic.bgActorFlags[0]=BGACTOR_IN_USE;dynamic.bgActors[0].actor=&moving;
    dynamic.bgActors[0].boundingSphere={{15,0,0},20};
    dynamic.bgActors[0].dynaLookup.floor.head=dynamic.bgActors[0].dynaLookup.ceiling.head=SS_NULL;
    dynamic.bgActors[0].dynaLookup.wall.head=0;
    dynamic.polyNodes.tbl=&node;dynamic.polyNodes.count=1;dynamic.polyList=&poly;dynamic.polyListMax=1;
    dynamic.vtxList=vertices;dynamic.vtxListMax=3;
    for(auto& v:vertices)v.x=15;
    okay&=WorldContact(context,player,{14.5f,0,0},1,hit)&&hit.normal[0]<0;
    auto* held=player->heldActor;player->heldActor=&moving;
    okay&=!WorldContact(context,player,{14.5f,0,0},1,hit);player->heldActor=held;
    auto* mount=player->rideActor;player->rideActor=&moving;
    okay&=!WorldContact(context,player,{14.5f,0,0},1,hit);player->rideActor=mount;
    dynamic.bgActorFlags[0]|=BGACTOR_COLLISION_DISABLED;
    okay&=!WorldContact(context,player,{14.5f,0,0},1,hit);
    return okay;
}
#endif
#endif
