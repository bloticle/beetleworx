#include "em1_motion_states.h"
static em2::em1::MotionStateContext context;
static em2::em1::SceneAdapterResult output;
static std::string error;
extern "C" __declspec(dllexport) void reset(){context={};}
extern "C" __declspec(dllexport) const char* reserve(const uint8_t* bytes,size_t size,uint32_t entities,uint32_t components){
 em2::em1::SceneAdapterResult scene;scene.bytes.assign(bytes,bytes+size);scene.entity_count=entities;scene.component_count=components;
 return em2::em1::ReserveMotionStateIds(scene,context,error)?nullptr:error.c_str();
}
extern "C" __declspec(dllexport) const char* adapt(const uint8_t* bytes,size_t size,uint32_t entities,uint32_t components,const uint8_t** out,size_t* length){
 em2::em1::SceneAdapterResult scene;scene.bytes.assign(bytes,bytes+size);scene.entity_count=entities;scene.component_count=components;em2::em1::MotionStateReport report;
 if(!em2::em1::AdaptLegacyMotionStates(scene,context,output,report,error))return error.c_str();
 *out=output.bytes.data();*length=output.bytes.size();return nullptr;
}
