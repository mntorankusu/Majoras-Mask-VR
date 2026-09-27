#pragma once
namespace {
ColliderCylinder trackedTorso{};ColliderSphere trackedExtremities[3]{};
Player* trackedBodyOwner=nullptr;int trackedBodyScene=-1;bool trackedBodyReady=false,trackedBodyQueued=false,trackedHandReady[2]{};
Vec3f trackedHead{},trackedPalms[2]{};std::chrono::steady_clock::time_point trackedBodyAt;
ColliderTris dekuGuard{};ColliderTrisElement dekuGuardElements[12]{};
mmvr::Matrix dekuGuardMatrix{},dekuGuardLocal{},dekuGuardPreviousLocal{};
float bodyRenderAlpha=1;Player* dekuGuardOwner=nullptr;unsigned dekuGuardFrame=0;Vec3f dekuGuardRoot{};bool dekuGuardQueued=false;
bool BodyProxy(const Collider* c){return c==&trackedTorso.base||c==&trackedExtremities[0].base||c==&trackedExtremities[1].base||c==&trackedExtremities[2].base;}
}
namespace mmvrgame {
void ClearBodyTracking(){trackedBodyReady=trackedBodyQueued=false;}
void RefreshBodyGeometry(Player* p){
 const float floor=p->actor.world.pos.y,top=std::max(floor+4,trackedHead.y-5);
 trackedTorso.dim.pos={(s16)trackedHead.x,(s16)floor,(s16)trackedHead.z};trackedTorso.dim.height=(s16)(top-floor);
 for(int i=0;i<3;++i){auto point=i?trackedPalms[i-1]:trackedHead;if(!i)point.y-=1;
  trackedExtremities[i].dim.worldSphere.center={(s16)point.x,(s16)point.y,(s16)point.z};}
}
void RecordBodyTracking(const mmvr::TrackingFrame& f,const mmvr::Matrix& view,const mmvr::Matrix& head){
 bodyRenderAlpha=std::clamp(f.visualAlpha,0.f,1.f);
 auto* play=gPlayState;auto* p=play?GET_PLAYER(play):nullptr;trackedBodyReady=p&&mmvr::FirstPersonRequested()&&FirstPersonFormAllowed(p)&&mmvr::InputFocused();
 if(!trackedBodyReady)return;trackedBodyOwner=p;trackedBodyScene=play->sceneId;trackedBodyAt=std::chrono::steady_clock::now();
 trackedHead={view.m[3][0],view.m[3][1]+head.m[3][1]*mmvr::WorldUnitsPerMetre(),view.m[3][2]};
 auto inverse=mmvr::InversePose(mmvr::PoseMatrix(f.origin));
 for(int h=0;h<2;++h){trackedHandReady[h]=f.handValid[h]&&f.handTracked[h];auto m=mmvr::Multiply(mmvr::PoseMatrix(f.hands[h]),inverse);
  const float units=mmvr::WorldUnitsPerMetre();
  m.m[3][0]=(m.m[3][0]-head.m[3][0])*units;m.m[3][1]*=units;m.m[3][2]=(m.m[3][2]-head.m[3][2])*units;m=mmvr::Multiply(m,view);
  trackedPalms[h]={m.m[3][0],m.m[3][1],m.m[3][2]};
 }
 if(trackedBodyQueued)RefreshBodyGeometry(p);
}
void QueueTrackedBody(PlayState* play,Player* p){
 trackedBodyQueued=false;for(auto& sphere:trackedExtremities)sphere={};auto& ctx=play->colChkCtx;
 if(!trackedBodyReady||trackedBodyOwner!=p||trackedBodyScene!=play->sceneId||!mmvr::FirstPersonRequested()||std::chrono::steady_clock::now()-trackedBodyAt>std::chrono::milliseconds(150))return;
 bool nativeQueued=false;for(int i=0;i<ctx.colACCount;++i)nativeQueued|=ctx.colAC[i]==&p->cylinder.base;
 if(!nativeQueued||ctx.colACCount+3>int(ARRAY_COUNT(ctx.colAC)))return;
 trackedTorso=p->cylinder;trackedTorso.dim.radius=8;trackedTorso.dim.yShift=0;
 const float floor=p->actor.world.pos.y,top=std::max(floor+4,trackedHead.y-5);
 trackedTorso.dim.pos={(s16)trackedHead.x,(s16)floor,(s16)trackedHead.z};trackedTorso.dim.height=(s16)(top-floor);
 mmvr::RemoveQueued(ctx.colAC,ctx.colACCount,&p->cylinder.base);
 CollisionCheck_SetAC(play,&ctx,&trackedTorso.base);
 for(int i=0;i<3;++i){if(i&&!trackedHandReady[i-1])continue;
  auto& sphere=trackedExtremities[i];sphere={};sphere.base=p->cylinder.base;sphere.base.shape=COLSHAPE_SPHERE;sphere.elem=p->cylinder.elem;
  auto point=i?trackedPalms[i-1]:trackedHead;if(!i)point.y-=1;
  sphere.dim.worldSphere.center={(s16)point.x,(s16)point.y,(s16)point.z};sphere.dim.worldSphere.radius=i?3:4;
  CollisionCheck_SetAC(play,&ctx,&sphere.base);
 }
 trackedBodyQueued=true;
}
void ResolveTrackedBody(PlayState* play,Player* p){
 if(!trackedBodyQueued)return;
 Collider* hit=nullptr;ColliderElement* element=nullptr;
 for(auto& sphere:trackedExtremities)if(sphere.base.acFlags&AC_HIT){hit=&sphere.base;element=&sphere.elem;break;}
 if(!hit&&(trackedTorso.base.acFlags&AC_HIT)){hit=&trackedTorso.base;element=&trackedTorso.elem;}
 if(hit){p->cylinder.base.acFlags|=AC_HIT;p->cylinder.base.ac=hit->ac;p->cylinder.elem=*element;
  // One native damage application even if a wide attack touched several body parts.
  for(auto& sphere:trackedExtremities)if(&sphere.base!=hit)sphere.base.acFlags&=~AC_HIT;
  if(&trackedTorso.base!=hit)trackedTorso.base.acFlags&=~AC_HIT;
  p->shieldQuad.base.acFlags&=~AC_BOUNCED;p->shieldCylinder.base.acFlags&=~AC_BOUNCED;
 }
}
mmvr::Matrix DekuGuardPose(PlayState* play,Player* p){
 if(!play||!p||p!=dekuGuardOwner||p->transformation!=PLAYER_FORM_DEKU||!(p->stateFlags1&PLAYER_STATE1_400000)||play->gameplayFrames-dekuGuardFrame>2)return {};
 auto local=dekuGuardLocal;
 if(dekuGuardPreviousLocal.m[3][3])
  for(int r=0;r<4;++r)for(int c=0;c<4;++c)
   local.m[r][c]=dekuGuardPreviousLocal.m[r][c]+(local.m[r][c]-dekuGuardPreviousLocal.m[r][c])*bodyRenderAlpha;
 auto head=FormHeadPose();if(head.m[3][3])return mmvr::Multiply(local,head);
 auto pose=dekuGuardMatrix;for(int c=0;c<3;++c)pose.m[3][c]+=(&p->actor.world.pos.x)[c]-(&dekuGuardRoot.x)[c];return pose;
}
mmvr::Matrix DekuGuardCorrection(PlayState* play,Player* p){
 auto current=DekuGuardPose(play,p);mmvr::Matrix inverse;
 if(!current.m[3][3]||!mmvr::InverseAffine(dekuGuardMatrix,inverse))return {};
 return mmvr::Multiply(inverse,current);
}
void QueueDekuGuard(PlayState* play,Player* p){
 dekuGuardQueued=false;
 if(p->transformation!=PLAYER_FORM_DEKU||!mmvr::FirstPersonRequested()||mmvr::GetSettings().Get(mmvr::Setting::PhysicalShield)<.5f)return;
 auto& ctx=play->colChkCtx;mmvr::RemoveQueued(ctx.colAC,ctx.colACCount,&p->shieldCylinder.base);
 if(!(p->stateFlags1&PLAYER_STATE1_400000)||dekuGuardOwner!=p||play->gameplayFrames-dekuGuardFrame>2)return;
 dekuGuard.base=p->shieldQuad.base;dekuGuard.base.shape=COLSHAPE_TRIS;dekuGuard.base.colMaterial=COL_MATERIAL_WOOD;dekuGuard.base.acFlags=AC_ON|AC_HARD|AC_TYPE_ENEMY;
 dekuGuard.count=12;dekuGuard.elements=dekuGuardElements;
 const auto guardPose=DekuGuardPose(play,p);
 Vec3f corners[8];for(int i=0;i<8;++i){float local[3]={i&1?1597.f:-1618.f,i&2?1085.f:-2734.f,i&4?1530.f:-1530.f};
  for(int c=0;c<3;++c){(&corners[i].x)[c]=guardPose.m[3][c];for(int r=0;r<3;++r)(&corners[i].x)[c]+=local[r]*guardPose.m[r][c];}}
 const int triangles[12][3]={{0,1,3},{0,3,2},{4,6,7},{4,7,5},{0,4,5},{0,5,1},{2,3,7},{2,7,6},{0,2,6},{0,6,4},{1,5,7},{1,7,3}};
 for(int i=0;i<12;++i){dekuGuardElements[i].base=p->shieldQuad.elem;dekuGuardElements[i].base.acElemFlags=ACELEM_ON;Collider_SetTrisVertices(&dekuGuard,i,&corners[triangles[i][0]],&corners[triangles[i][1]],&corners[triangles[i][2]]);}
 CollisionCheck_SetAC(play,&ctx,&dekuGuard.base);for(int i=ctx.colACCount-1;i>0;--i)if(ctx.colAC[i]==&dekuGuard.base)std::swap(ctx.colAC[i],ctx.colAC[i-1]);dekuGuardQueued=true;
}
}
extern "C" void MMVR_RecordDekuGuard(PlayState* play,Player* p){
 if(!play||!p||!mmvr::FirstPersonRequested())return;
 const bool continuous=dekuGuardOwner==p&&play->gameplayFrames-dekuGuardFrame==1&&dekuGuardLocal.m[3][3];
 if(play->gameplayFrames!=dekuGuardFrame)dekuGuardPreviousLocal=continuous?dekuGuardLocal:mmvr::Matrix{};
 MtxF m;Matrix_Get(&m);std::memcpy(&dekuGuardMatrix,&m,sizeof(m));dekuGuardOwner=p;dekuGuardFrame=play->gameplayFrames;dekuGuardRoot=p->actor.world.pos;
 auto reference=mmvr::YawPose(float(p->actor.shape.rot.y)*3.14159265359f/32768.f+3.14159265359f,p->actor.world.pos.x,p->actor.world.pos.y+mmvrgame::FormEyeHeight(p),p->actor.world.pos.z);
 dekuGuardLocal=mmvr::Multiply(dekuGuardMatrix,mmvr::InversePose(reference));
}

extern "C" void MMVR_BindDekuGuard(const void* address){mmvr::SetDekuGuardMatrix(address);}

#if defined(MMVR_STATE_NATIVE_BACKEND)
#include "NativeStateFields.h"
// Pending native collision results may point into these VR-owned shapes. Only
// their collider and gameplay pose bytes are owned here; device objects are excluded.
extern "C" void MMVR_VisitVrBodyCollisionState(MMVR_StateSink* sink) {
 mmvrgame::NativeStateField(sink,"vr/body/trackedBodyOwner",trackedBodyOwner);
 mmvrgame::NativeStateField(sink,"vr/body/trackedBodyScene",trackedBodyScene);
 mmvrgame::NativeStateField(sink,"vr/body/trackedBodyReady",trackedBodyReady);
 mmvrgame::NativeStateField(sink,"vr/body/trackedBodyQueued",trackedBodyQueued);
 mmvrgame::NativeStateField(sink,"vr/body/trackedHandReady",trackedHandReady);
 mmvrgame::NativeStateField(sink,"vr/body/trackedHead",trackedHead);
 mmvrgame::NativeStateField(sink,"vr/body/trackedPalms",trackedPalms);
 mmvrgame::NativeStateField(sink,"vr/body/trackedBodyAt",trackedBodyAt);
 mmvrgame::NativeStateField(sink,"vr/body/dekuGuardMatrix",dekuGuardMatrix);
 mmvrgame::NativeStateField(sink,"vr/body/dekuGuardLocal",dekuGuardLocal);
 mmvrgame::NativeStateField(sink,"vr/body/dekuGuardPreviousLocal",dekuGuardPreviousLocal);
 mmvrgame::NativeStateField(sink,"vr/body/dekuGuardOwner",dekuGuardOwner);
 mmvrgame::NativeStateField(sink,"vr/body/dekuGuardFrame",dekuGuardFrame);
 mmvrgame::NativeStateField(sink,"vr/body/dekuGuardRoot",dekuGuardRoot);
 mmvrgame::NativeStateField(sink,"vr/body/dekuGuardQueued",dekuGuardQueued);
 sink->block(sink->context,"vr/collision/torso",&trackedTorso,sizeof(trackedTorso));
 MMVR_StateVisitColliderCylinder(sink,&trackedTorso);
 const char* names[]={"vr/collision/head","vr/collision/left-hand","vr/collision/right-hand"};
 for(int i=0;i<3;++i){sink->block(sink->context,names[i],&trackedExtremities[i],sizeof(trackedExtremities[i]));MMVR_StateVisitColliderSphere(sink,&trackedExtremities[i]);}
 sink->block(sink->context,"vr/collision/deku-guard",&dekuGuard,sizeof(dekuGuard));
 sink->block(sink->context,"vr/collision/deku-guard-elements",dekuGuardElements,sizeof(dekuGuardElements));
 MMVR_StateVisitColliderTris(sink,&dekuGuard);
}
#endif
