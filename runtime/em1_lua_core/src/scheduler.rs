use mlua::{Function,HookTriggers,Lua,MultiValue,Result,Thread,ThreadStatus,Value,VmState};
use std::{collections::VecDeque,sync::{Arc,Mutex,atomic::{AtomicU32,Ordering}}};

pub const YIELD_REGISTRY:&str="em1.native.scheduler_yield";
const MAX_TASKS:usize=256;
const MAX_RESUMES:usize=32;
#[derive(Clone)]
struct Task { thread:Thread,deadline:f64,after_tick:u64,args:Option<MultiValue>,name:String }
#[derive(Default,Clone)]
pub struct Scheduler { tasks:VecDeque<Task>,seconds:f64,tick:u64,stats:Stats,dispatch:Dispatcher }
#[repr(C)]
#[derive(Default,Clone,Copy,Debug,PartialEq,Eq)]
pub struct Stats {pub submitted:u64,pub resumes:u64,pub completed:u64,pub failed:u64,pub cancelled:u64,pub pending:u64}
fn error(message:&str)->mlua::Error {mlua::Error::RuntimeError(message.into())}

// The active dispatch frame is independent of Em1LuaWorld and Scheduler's
// mutable tick borrow. No mutex is retained while a Lua child is resumed.
#[derive(Clone,Default)]
pub struct Dispatcher(Arc<Mutex<Option<Arc<Frame>>>>);
struct Frame {seconds:f64,tick:u64,instructions:Arc<AtomicU32>,resumes:AtomicU32,depth:AtomicU32,data:Mutex<FrameData>}
#[derive(Default)]struct FrameData {live:usize,inbox:VecDeque<Task>,stats:Stats,failures:Vec<String>}
struct DispatchScope(Dispatcher);
impl Drop for DispatchScope {fn drop(&mut self){*self.0.0.lock().unwrap_or_else(|e|e.into_inner())=None;}}
struct Depth<'a>(&'a AtomicU32);
impl Drop for Depth<'_>{fn drop(&mut self){self.0.fetch_sub(1,Ordering::Relaxed);}}
enum Outcome {Waiting,Retired}
impl Dispatcher {
 fn enter(&self,seconds:f64,tick:u64,live:usize)->Result<(Arc<Frame>,DispatchScope)>{
  let mut active=self.0.lock().unwrap_or_else(|e|e.into_inner());
  if active.is_some(){return Err(error("Nested original scheduler tick"));}
  let frame=Arc::new(Frame{seconds,tick,instructions:Arc::new(AtomicU32::new(0)),resumes:AtomicU32::new(0),depth:AtomicU32::new(0),data:Mutex::new(FrameData{live,..FrameData::default()})});
  *active=Some(frame.clone());Ok((frame,DispatchScope(self.clone())))
 }
 fn active(&self)->Result<Arc<Frame>>{self.0.lock().unwrap_or_else(|e|e.into_inner()).clone().ok_or_else(||error("FireThread requires an active original simulation dispatch"))}
}
impl Frame {
 fn available(&self)->bool{self.resumes.load(Ordering::Relaxed)<MAX_RESUMES as u32 && self.instructions.load(Ordering::Relaxed)<100}
 fn retire(&self,failed:Option<String>){let mut data=self.data.lock().unwrap_or_else(|e|e.into_inner());data.live-=1;
  if let Some(message)=failed{data.stats.failed=data.stats.failed.saturating_add(1);data.failures.push(message);}else{data.stats.completed=data.stats.completed.saturating_add(1);}}
 fn resume(&self,task:&mut Task)->Result<Outcome>{
  if !self.available(){return Err(error("Original nested thread shared resume/instruction budget exceeded"));}
  let depth=self.depth.fetch_add(1,Ordering::Relaxed);let _depth=Depth(&self.depth);
  if depth>=16{return Err(error("Original FireThread nesting budget exceeded"));}
  let budget=self.instructions.clone();task.thread.set_hook(HookTriggers::new().every_nth_instruction(1000),move |_,_|{
   if budget.fetch_add(1,Ordering::Relaxed)>=99{Err(error("EM1 callback instruction budget exceeded"))}else{Ok(VmState::Continue)}
  })?;
  self.resumes.fetch_add(1,Ordering::Relaxed);{let mut data=self.data.lock().unwrap_or_else(|e|e.into_inner());data.stats.resumes=data.stats.resumes.saturating_add(1);}
  match task.thread.resume::<MultiValue>(task.args.take().unwrap_or_default()){
   Ok(values) if task.thread.status()==ThreadStatus::Resumable=>{
    let delay=match values.front(){Some(Value::Number(v))=>Some(*v),Some(Value::Integer(v))=>Some(*v as f64),_=>None};
    if let Some(delay)=delay.filter(|v|v.is_finite() && *v>=0. && (self.seconds+v).is_finite()).filter(|_|values.len()==1){
     task.deadline=self.seconds+delay;task.after_tick=self.tick;Ok(Outcome::Waiting)
    }else{self.retire(Some(format!("{}: unsupported EM1 coroutine yield",task.name)));Ok(Outcome::Retired)}
   },
   Ok(_)=>{self.retire(None);Ok(Outcome::Retired)},
   Err(error)=>{self.retire(Some(format!("{}: {error}",task.name)));Ok(Outcome::Retired)},
  }
 }
 fn child(&self,vm:&Lua,function:Value,args:MultiValue,factory:&Function)->Result<()>{
  // Lua's native coroutine entry can fail by trying to call a non-function;
  // isolate that failure inside the child instead of throwing through parent.
  let callback=match function{Value::Function(function)=>function,value=>factory.call::<Function>(value)?};
  let mut task=Task{thread:vm.create_thread(callback)?,deadline:self.seconds,after_tick:self.tick,args:Some(args),name:"FireThread child".into()};
  if !self.available() || self.depth.load(Ordering::Relaxed)>=16{return Err(error("Original FireThread dispatch budget exceeded"));}
  {let mut data=self.data.lock().unwrap_or_else(|e|e.into_inner());if data.live>=MAX_TASKS{return Err(error("EM1 coroutine limit reached"));}data.live+=1;data.stats.submitted=data.stats.submitted.saturating_add(1);}
  match self.resume(&mut task){Ok(Outcome::Waiting)=>self.data.lock().unwrap_or_else(|e|e.into_inner()).inbox.push_back(task),Ok(Outcome::Retired)=>{},Err(error)=>{
   // Host admission/setup failure is explicit; it is not an accepted child.
   let mut data=self.data.lock().unwrap_or_else(|e|e.into_inner());data.live-=1;data.stats.submitted-=1;return Err(error);
  }}Ok(())
 }
 fn drain(&self,tasks:&mut VecDeque<Task>){tasks.append(&mut self.data.lock().unwrap_or_else(|e|e.into_inner()).inbox);}
}
pub fn install_fire_thread(vm:&Lua)->Result<Dispatcher>{
 let dispatch=Dispatcher::default();let access=dispatch.clone();
 let factory:Function=vm.load("return function(value) return function(...) return value(...) end end").eval()?;
 vm.globals().raw_set("FireThread",vm.create_function(move |vm,mut args:MultiValue|{
  let function=args.pop_front().ok_or_else(||error("Original FireThread requires at least a callable argument"))?;
  access.active()?.child(vm,function,args,&factory)
 })?)?;Ok(dispatch)
}
pub fn install_wait(vm:&Lua)->Result<()> {
    // Lua 5.1 cannot yield through an arbitrary Rust/C callback. Keep the
    // original coroutine.yield in a private upvalue of this Lua wrapper.
    let factory:Function=vm.load(r#"
      local kind,fail,huge=type,error,math.huge
      return function(yield)
        return function(seconds)
          if kind(seconds)~='number' or seconds~=seconds or seconds<0 or seconds==huge then
            fail('wait expects finite nonnegative seconds')
          end
          return yield(seconds)
        end
      end
    "#).eval()?;
    let wait:Function=factory.call(vm.named_registry_value::<Function>(YIELD_REGISTRY)?)?;
    vm.globals().raw_set("wait",wait.clone())?;vm.globals().raw_set("Wait",wait)?;
    vm.unset_named_registry_value(YIELD_REGISTRY)?;
    Ok(())
}
impl Scheduler {
    #[cfg(test)]
    pub fn start(&mut self,vm:&Lua,name:&str)->Result<()> {
        self.start_args(vm,name,MultiValue::new())
    }
    pub fn start_args(&mut self,vm:&Lua,name:&str,args:MultiValue)->Result<()> {
        self.start_args_after(vm,name,args,0.0)
    }
    pub fn start_args_after(&mut self,vm:&Lua,name:&str,args:MultiValue,delay:f64)->Result<()> {
        if !delay.is_finite() || delay<0.0 || !(self.seconds+delay).is_finite(){return Err(error("Invalid authored callback delay"));}
        if self.tasks.len()>=MAX_TASKS{return Err(error("EM1 coroutine limit reached"));}
        let callback:Function=vm.globals().raw_get(name)?;
        self.tasks.push_back(Task{thread:vm.create_thread(callback)?,deadline:self.seconds+delay,after_tick:self.tick,args:Some(args),name:name.to_owned()});
        self.stats.submitted=self.stats.submitted.saturating_add(1);
        Ok(())
    }
    pub fn stats(&self)->Stats{Stats{pending:self.tasks.len() as u64,..self.stats}}
    pub fn cancel(&mut self){self.stats.cancelled=self.stats.cancelled.saturating_add(self.tasks.len() as u64);self.tasks.clear();}
    pub fn with_dispatch(dispatch:Dispatcher)->Self{Self{dispatch,..Self::default()}}
    pub fn tick(&mut self,delta:f64)->Result<Vec<String>> {
        if !delta.is_finite() || delta<0.0 || !(self.seconds+delta).is_finite(){return Err(error("Invalid simulation delta"));}
        if delta==0.0{return Ok(vec![]);}
        self.seconds+=delta;self.tick=self.tick.checked_add(1).ok_or_else(||error("Simulation tick overflow"))?;
        if self.tasks.is_empty(){return Ok(vec![]);}
        let (frame,_scope)=self.dispatch.enter(self.seconds,self.tick,self.tasks.len())?;
        for _ in 0..self.tasks.len(){
            let mut task=self.tasks.pop_front().unwrap();
            if !frame.available(){self.tasks.push_front(task);break;}
            if task.deadline>self.seconds || task.after_tick>=self.tick{self.tasks.push_back(task);continue;}
            let outcome=frame.resume(&mut task)?;
            // Native child is inserted when it first yields, before a parent
            // that subsequently yields. No child is first-run next frame.
            frame.drain(&mut self.tasks);
            if matches!(outcome,Outcome::Waiting){self.tasks.push_back(task);}
        }
        frame.drain(&mut self.tasks);let mut data=frame.data.lock().unwrap_or_else(|e|e.into_inner());
        self.stats.submitted=self.stats.submitted.saturating_add(data.stats.submitted);
        self.stats.resumes=self.stats.resumes.saturating_add(data.stats.resumes);
        self.stats.completed=self.stats.completed.saturating_add(data.stats.completed);
        self.stats.failed=self.stats.failed.saturating_add(data.stats.failed);
        debug_assert_eq!(data.live,self.tasks.len());Ok(std::mem::take(&mut data.failures))
    }

}

#[cfg(test)]
mod tests {
    use super::*;
    fn setup(source:&str)->(Lua,Scheduler){let vm=super::super::definition_vm().unwrap();install_wait(&vm).unwrap();vm.load(source).exec().unwrap();(vm,Scheduler::default())}
    fn nested(source:&str)->(Lua,Scheduler){let vm=super::super::definition_vm().unwrap();install_wait(&vm).unwrap();let dispatch=install_fire_thread(&vm).unwrap();vm.load(source).exec().unwrap();(vm,Scheduler::with_dispatch(dispatch))}
    #[test]fn fire_thread_resumes_immediately_preserves_values_and_retains_yielded_children(){
        let(vm,mut scheduler)=nested("trace=''; shared={value=41}; callback=function()return 17 end; function child(t,f,n,last) assert(t==shared and f==callback and n==nil and last==9); trace=trace..'c'; FireThread(function()trace=trace..'g' end); wait(.2); trace=trace..'d' end; function event() trace='p'; FireThread(child,shared,callback,nil,9); trace=trace..'a' end");
        scheduler.start(&vm,"event").unwrap();assert!(scheduler.tick(0.1).unwrap().is_empty());assert_eq!(vm.globals().get::<String>("trace").unwrap(),"pcga");assert_eq!((scheduler.stats().submitted,scheduler.stats().resumes,scheduler.stats().completed,scheduler.stats().pending),(3,3,2,1));
        scheduler.tick(0.1).unwrap();assert_eq!(vm.globals().get::<String>("trace").unwrap(),"pcga");scheduler.tick(0.1).unwrap();assert_eq!(vm.globals().get::<String>("trace").unwrap(),"pcgad");assert_eq!((scheduler.stats().completed,scheduler.stats().pending),(3,0));
        assert!(vm.load("FireThread(child)").exec().is_err());
    }
    #[test]fn child_errors_and_noncallable_entries_do_not_abort_the_parent(){
        let(vm,mut scheduler)=nested("trace=''; function event() trace='p'; FireThread(function()error('original child error') end); trace=trace..'a'; FireThread(nil); trace=trace..'b' end");
        scheduler.start(&vm,"event").unwrap();assert_eq!(scheduler.tick(0.1).unwrap().len(),2);assert_eq!(vm.globals().get::<String>("trace").unwrap(),"pab");assert_eq!((scheduler.stats().submitted,scheduler.stats().completed,scheduler.stats().failed,scheduler.stats().pending),(3,1,2,0));
    }
    #[test]fn zero_wait_does_not_resume_twice_and_cancel_retires_owned_child(){
        let(vm,mut scheduler)=nested("trace=''; function event() FireThread(function() trace='a'; wait(0); trace=trace..'b' end) end");
        scheduler.start(&vm,"event").unwrap();scheduler.tick(0.1).unwrap();assert_eq!(vm.globals().get::<String>("trace").unwrap(),"a");scheduler.tick(0.).unwrap();assert_eq!(vm.globals().get::<String>("trace").unwrap(),"a");scheduler.tick(0.1).unwrap();assert_eq!(vm.globals().get::<String>("trace").unwrap(),"ab");
        scheduler.start(&vm,"event").unwrap();scheduler.tick(0.1).unwrap();scheduler.cancel();scheduler.tick(10.).unwrap();assert_eq!(vm.globals().get::<String>("trace").unwrap(),"a");assert_eq!(scheduler.stats().cancelled,1);
    }
    #[test]fn nested_children_share_instruction_resume_depth_and_task_budgets(){
        let(vm,mut scheduler)=nested("function recurse() FireThread(recurse) end; function event() FireThread(recurse); survived=true end; function good()good_count=(good_count or 0)+1 end; function many() for i=1,1000 do FireThread(function() end) end end; function spin_child() FireThread(function()while true do end end); after_spin=true end");
        scheduler.start(&vm,"event").unwrap();assert_eq!(scheduler.tick(0.1).unwrap().len(),1);assert!(vm.globals().get::<bool>("survived").unwrap());assert!(scheduler.stats().submitted<=16);assert_eq!(scheduler.stats().pending,0);
        scheduler.start(&vm,"many").unwrap();scheduler.start(&vm,"good").unwrap();assert_eq!(scheduler.tick(0.1).unwrap().len(),1);assert_eq!(scheduler.stats().pending,1);assert!(vm.globals().get::<Value>("good_count").unwrap().is_nil());scheduler.tick(0.1).unwrap();assert_eq!(vm.globals().get::<u32>("good_count").unwrap(),1);
        scheduler.start(&vm,"spin_child").unwrap();scheduler.start(&vm,"good").unwrap();assert_eq!(scheduler.tick(0.1).unwrap().len(),1);assert!(vm.globals().get::<bool>("after_spin").unwrap());assert_eq!(scheduler.stats().pending,1);scheduler.tick(0.1).unwrap();assert_eq!(vm.globals().get::<u32>("good_count").unwrap(),2);
    }
    #[test]fn stats_distinguish_submission_wait_resume_completion_failure_and_cancel(){
        let(vm,mut scheduler)=setup("function waiting() wait(1); wait(0) end; function broken() error('failed') end; function endless() while true do end end");
        scheduler.start(&vm,"waiting").unwrap();assert!(scheduler.start(&vm,"missing").is_err());
        assert_eq!(scheduler.stats(),Stats{submitted:1,pending:1,..Stats::default()});
        scheduler.tick(0.0).unwrap();assert_eq!(scheduler.stats().resumes,0);
        scheduler.tick(0.1).unwrap();assert_eq!(scheduler.stats().completed,0);assert_eq!(scheduler.stats().resumes,1);
        scheduler.tick(0.5).unwrap();assert_eq!(scheduler.stats().resumes,1);
        scheduler.tick(0.5).unwrap();assert_eq!(scheduler.stats().completed,0);assert_eq!(scheduler.stats().resumes,2);
        scheduler.tick(0.1).unwrap();assert_eq!(scheduler.stats().completed,1);assert_eq!(scheduler.stats().resumes,3);
        scheduler.start(&vm,"broken").unwrap();assert_eq!(scheduler.tick(0.1).unwrap().len(),1);
        scheduler.start(&vm,"endless").unwrap();assert_eq!(scheduler.tick(0.1).unwrap().len(),1);
        scheduler.start(&vm,"waiting").unwrap();scheduler.tick(0.1).unwrap();scheduler.cancel();scheduler.cancel();
        assert_eq!(scheduler.stats(),Stats{submitted:4,resumes:6,completed:1,failed:2,cancelled:1,pending:0});
        scheduler.tick(10.0).unwrap();assert_eq!(scheduler.stats().completed,1);
    }
    #[test]fn timing_pause_zero_wait_and_cancel(){
        for fps in [30,60,120,144] {
            let(vm,mut scheduler)=setup("phase=0; function event() phase=1; wait(1); phase=2; Wait(0); phase=3 end");
            scheduler.start(&vm,"event").unwrap();scheduler.tick(0.0).unwrap();assert_eq!(vm.globals().get::<i32>("phase").unwrap(),0);
            let dt=1.0/fps as f64;scheduler.tick(dt).unwrap();assert_eq!(vm.globals().get::<i32>("phase").unwrap(),1);
            for _ in 0..fps-1{assert!(scheduler.tick(dt).unwrap().is_empty());}
            assert_eq!(vm.globals().get::<i32>("phase").unwrap(),1);
            scheduler.tick(dt+1e-10).unwrap();assert_eq!(vm.globals().get::<i32>("phase").unwrap(),2);
            scheduler.tick(0.0).unwrap();assert_eq!(vm.globals().get::<i32>("phase").unwrap(),2);
            scheduler.tick(dt).unwrap();assert_eq!(vm.globals().get::<i32>("phase").unwrap(),3);
            scheduler.start(&vm,"event").unwrap();scheduler.cancel();scheduler.tick(dt).unwrap();assert_eq!(vm.globals().get::<i32>("phase").unwrap(),3);
        }
    }
    #[test]fn limits_errors_and_recovery(){
        let(vm,mut scheduler)=setup("done=0; function spin() while true do end end; function ok() done=done+1 end; function bad() wait(-1) end");
        scheduler.start(&vm,"spin").unwrap();scheduler.start(&vm,"ok").unwrap();assert_eq!(scheduler.tick(0.1).unwrap().len(),1);
        assert!(scheduler.tick(0.1).unwrap().is_empty());assert_eq!(vm.globals().get::<i32>("done").unwrap(),1);
        scheduler.start(&vm,"bad").unwrap();assert_eq!(scheduler.tick(0.1).unwrap().len(),1);
        for delta in [-1.0,f64::NAN,f64::INFINITY] {assert!(scheduler.tick(delta).is_err());}
        assert!(scheduler.start(&vm,"missing").is_err());
        for _ in 0..MAX_TASKS {scheduler.start(&vm,"ok").unwrap();}assert!(scheduler.start(&vm,"ok").is_err());
        scheduler.tick(0.1).unwrap();assert_eq!(vm.globals().get::<i32>("done").unwrap(),1+MAX_RESUMES as i32);
    }
    #[test]fn authored_delays_own_arguments_pause_cancel_and_do_not_change_wait_deadlines(){
        let(vm,mut scheduler)=setup("seen=''; function delayed(value) seen=seen..value; wait(0.25); seen=seen..'w' end; function immediate() seen=seen..'i' end");
        let args=MultiValue::from_vec(vec![Value::String(vm.create_string("a").unwrap())]);
        scheduler.start_args_after(&vm,"delayed",args,0.5).unwrap();
        scheduler.start(&vm,"immediate").unwrap();
        scheduler.tick(0.0).unwrap();assert_eq!(vm.globals().get::<String>("seen").unwrap(),"");
        scheduler.tick(0.25).unwrap();assert_eq!(vm.globals().get::<String>("seen").unwrap(),"i");
        scheduler.tick(0.0).unwrap();assert_eq!(vm.globals().get::<String>("seen").unwrap(),"i");
        scheduler.tick(0.25).unwrap();assert_eq!(vm.globals().get::<String>("seen").unwrap(),"ia");
        scheduler.tick(0.125).unwrap();assert_eq!(vm.globals().get::<String>("seen").unwrap(),"ia");
        scheduler.tick(0.125).unwrap();assert_eq!(vm.globals().get::<String>("seen").unwrap(),"iaw");
        for delay in [-1.0,f64::NAN,f64::INFINITY]{assert!(scheduler.start_args_after(&vm,"immediate",MultiValue::new(),delay).is_err());}
        scheduler.start_args_after(&vm,"immediate",MultiValue::new(),10.0).unwrap();scheduler.cancel();
        scheduler.tick(20.0).unwrap();assert_eq!(vm.globals().get::<String>("seen").unwrap(),"iaw");
    }
}

