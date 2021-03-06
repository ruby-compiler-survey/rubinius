#include <stdlib.h>

#include "config.h"
#include "paths.h"
#include "debug.h"

#include "c_api.hpp"
#include "config_parser.hpp"
#include "configuration.hpp"
#include "console.hpp"
#include "diagnostics.hpp"
#include "environment.hpp"
#include "exception.hpp"
#include "machine.hpp"
#include "machine_compiler.hpp"
#include "memory.hpp"
#include "on_stack.hpp"
#include "signals.hpp"
#include "thread_nexus.hpp"
#include "type_info.hpp"

#include "class/array.hpp"
#include "class/code_db.hpp"
#include "class/fiber.hpp"
#include "class/fixnum.hpp"
#include "class/thread.hpp"
#include "class/unwind_state.hpp"

#include "diagnostics/codedb.hpp"
#include "diagnostics/collector.hpp"
#include "diagnostics/memory.hpp"
#include "diagnostics/machine.hpp"
#include "diagnostics/profiler.hpp"
#include "diagnostics/timing.hpp"

#include "memory/header.hpp"
#include "memory/collector.hpp"

#include "sodium/randombytes.h"

#include "missing/gettid.h"

#include <sys/stat.h>

namespace rubinius {
  std::mutex Machine::_waiting_mutex_;
  std::mutex Machine::_halting_mutex_;
  std::condition_variable Machine::_waiting_condition_;

  MachineState::MachineState()
    : _start_time_(get_current_time())
    , _hash_seed_(randombytes_random())
    , _phase_(eBooting)
  {
  }

  void MachineState::set_start_time() {
    _start_time_ = get_current_time();
  }

  double MachineState::run_time() {
    return timer::time_elapsed_seconds(_start_time_);
  }

  Machine::Machine(int argc, char** argv)
    : _machine_state_(new MachineState())
    , _logger_(nullptr)
    , _thread_nexus_(new ThreadNexus(_halting_mutex_, _waiting_mutex_, _waiting_condition_))
    , _configuration_(new Configuration())
    , _environment_(new Environment(argc, argv, this))
    , _diagnostics_(new Diagnostics(_configuration_))
    , _memory_(new Memory(_environment_->state, _configuration_))
    , _collector_(new memory::Collector())
    , _signals_(new Signals())
    , _codedb_(nullptr)
    , _c_api_(new C_API())
    , _compiler_(nullptr)
    , _debugger_(nullptr)
    , _profiler_(new Profiler())
    , _console_(new Console(_environment_->state))
  {
    _environment_->initialize();

    if(configuration()->log_lifetime.value) {
      signals()->print_machine_info(logger::write);
      signals()->print_process_info(logger::write);

      logger::write("process: boot stats: " \
          "fields %lldus " \
          "main thread: %lldus " \
          "ontology: %lldus " \
          "platform: %lldus",
          diagnostics()->boot_metrics()->fields_us,
          diagnostics()->boot_metrics()->main_thread_us,
          diagnostics()->boot_metrics()->ontology_us,
          diagnostics()->boot_metrics()->platform_us);
    }
  }

  Machine::~Machine() {
    if(_machine_state_) halt();
  }

  Diagnostics* Machine::start_diagnostics(STATE) {
    if(state->configuration()->diagnostics_target.value.compare("none")) {
      _diagnostics_->start_reporter(state);

      _diagnostics_->boot_metrics()->start_reporting(state);
      _diagnostics_->codedb_metrics()->start_reporting(state);
      _diagnostics_->collector_metrics()->start_reporting(state);
      _diagnostics_->memory_metrics()->start_reporting(state);
      // _diagnostics_->profiler()->start_reporting(state);
    }

    return _diagnostics_;
  }

  void Machine::report_diagnostics(diagnostics::Diagnostic* diagnostic) {
    if(_diagnostics_) {
      _diagnostics_->report(diagnostic);
    }
  }


  /* TODO:
   *
   * [x] 1. Add Machine to SharedState;
   * [x] 1. Pass Machine through Environment to SharedState
   * [x] 1. Create MachineState;
   * [x] 1. Move SharedState items for env into Environment;
   * [ ] 1. Create stateful Logger;
   * [ ] 1. Create Random;
   * [x] 1. Move Configuration to Machine;
   * [ ] 1. Consolidate ConfigParse with Configuration;
   * [x] 1. Move Diagnostics to Machine;
   * [x] 1. Move ThreadNexus to Machine;
   * [x] 1. Move MachineThreads to Machine;
   * [x] 1. Move Memory to Machine;
   * [x] 1. Move Collector to Machine;
   * [x] 1. Move Signals to Machine;
   * [ ] 1. Move CodeDB to Machine;
   * [x] 1. Move C-API to Machine;
   * [ ] 1. Move Debugger to Machine;
   * [x] 1. Move Compiler to Machine;
   * [x] 1. Move Profiler to Machine;
   * [x] 1. Move Console to Machine;
   * [x] 1. Move SymbolTable into Memory;
   * [x] 1. Move Globals into Memory;
   * [x] 1. Create ThreadState to replace State;
   * [x] 1. Create ThreadState instances for new threads;
   * [x] 1. Consolidate and remove all extra State objects;
   * [x] 1. Merge VM into ThreadState;
   * [x] 1. Switch back to booting on main thread;
   */
  void Machine::boot() {
    // TODO: Machine
    ThreadState* state = environment()->state;

    environment()->setup_cpp_terminate();

    // TODO: Machine
    // _console_->start(_environment_->state);
    // state->shared().start_compiler(state);
    // state->diagnostics().start_diagnostics(state);

    MachineException::guard(state, false, [&]{
        if(const char* var = getenv("RBX_OPTIONS")) {
          environment()->load_string(var);
        }

        if(const char* path = getenv("RBX_OPTFILE")) {
          environment()->load_conf(path);
        }

        // -*-*-*-
        std::string codedb_path = environment()->system_prefix() + RBX_CODEDB_PATH;

        {
          timer::StopWatch<timer::microseconds> timer(
              diagnostics()->boot_metrics()->platform_us);

          environment()->load_platform_conf(codedb_path);
        }

        state->managed_phase(state);

        {
          timer::StopWatch<timer::microseconds> timer(
              diagnostics()->boot_metrics()->fields_us);

          TypeInfo::auto_learn_fields(state);
        }

        {
          timer::StopWatch<timer::microseconds> timer(
              diagnostics()->boot_metrics()->ontology_us);

          state->bootstrap_ontology(state);
        }

        environment()->load_command_line(state);

        /* TODO: Machine
        // Start the main Ruby thread.
        Thread* main = 0;
        OnStack<1> os(state, main);

        {
          timer::StopWatch<timer::microseconds> timer(
              diagnostics()->boot_metrics()->main_thread_us);

          main = Thread::create(state, state, Thread::main_thread);
          main->start_thread(state, Thread::run);
        }
        */

        // TODO: GC improve this
        collector()->start(state);

        signals()->start(state);

        state->metrics()->start_reporting(state);

        state->managed_phase(state);

        Thread::create(state, state);

        state->thread()->pid(state, Fixnum::from(gettid()));

        environment()->load_core(state);

        Object* loader_class = G(rubinius)->get_const(state, state->symbol("Loader"));
        if(loader_class->nil_p()) {
          state->environment()->missing_core("unable to find class Rubinius::Loader");
          return;
        }

        if(Object* loader = loader_class->send(state, state->symbol("new"))) {
          machine_state()->set_loader(loader);
        } else {
          state->environment()->missing_core("unable to instantiate Rubinius::Loader");
          return;
        }

        // TODO: Machine
        // Enable the JIT after the core library has loaded
        // G(jit)->enable(state);

        machine_state()->loader()->send(state, state->symbol("main"));

        if(state->unwind_state()->raise_reason() == cSystemExit) {
          halt(state, state->unwind_state()->raise_value());
        }
      });
  }

  void Machine::before_fork(STATE) {
  }

  void Machine::after_fork_parent(STATE) {
  }

  void Machine::after_fork_child(STATE) {
    logger::reset();

    _machine_state_->set_start_time();

    state->after_fork_child();

    _thread_nexus_->after_fork_child(state);

    _environment_->after_fork_child(state);

    _memory_->after_fork_child(state);
    _collector_->after_fork_child(state);
    _signals_->after_fork_child(state);
    _c_api_->after_fork_child(state);
    _console_->after_fork_child(state);
  }

  /* TODO
  jit::MachineCompiler* Machine::start_compiler(STATE) {
    if(!compiler_) {
      if(state->configuration()->jit_enabled.value) {
        compiler_ = new jit::MachineCompiler(state);
      }
    }

    return compiler_;
  }
   */

  Array* Machine::vm_threads(STATE) {
    std::lock_guard<std::mutex> guard(thread_nexus()->threads_mutex());

    Array* threads = Array::create(state, 0);

    for(ThreadList::iterator i = thread_nexus()->threads()->begin();
        i != thread_nexus()->threads()->end();
        ++i)
    {
      if(ThreadState* thread_state = *i) {
        Thread *thread = thread_state->thread();
        if(thread_state->kind() == ThreadState::eThread
            &&!thread->nil_p() && !thread->thread_state()->zombie_p()) {
          threads->append(state, thread);
        }
      }
    }

    return threads;
  }

  Fixnum* Machine::vm_threads_count(STATE) {
    std::lock_guard<std::mutex> guard(thread_nexus()->threads_mutex());

    intptr_t count = 0;

    for(ThreadList::iterator i = thread_nexus()->threads()->begin();
        i != thread_nexus()->threads()->end();
        ++i)
    {
      if(ThreadState* thread_state = *i) {
        Thread *thread = thread_state->thread();
        if(thread_state->kind() == ThreadState::eThread
            &&!thread->nil_p() && !thread->thread_state()->zombie_p()) {
          count++;
        }
      }
    }

    return Fixnum::from(count);
  }

  Array* Machine::vm_fibers(STATE) {
    std::lock_guard<std::mutex> guard(thread_nexus()->threads_mutex());

    Array* fibers = Array::create(state, 0);

    for(ThreadList::iterator i = thread_nexus()->threads()->begin();
        i != thread_nexus()->threads()->end();
        ++i)
    {
      if(ThreadState* thread_state = *i) {
        if(thread_state->kind() == ThreadState::eFiber
            && !thread_state->fiber()->nil_p()
            && thread_state->fiber()->status() != Fiber::eDead) {
          fibers->append(state, thread_state->fiber());
        }
      }
    }

    return fibers;
  }

  Fixnum* Machine::vm_fibers_count(STATE) {
    std::lock_guard<std::mutex> guard(thread_nexus()->threads_mutex());

    intptr_t count = 0;

    for(ThreadList::iterator i = thread_nexus()->threads()->begin();
        i != thread_nexus()->threads()->end();
        ++i)
    {
      if(ThreadState* thread_state = *i) {
        if(thread_state->kind() == ThreadState::eFiber
            && !thread_state->fiber()->nil_p()
            && thread_state->fiber()->status() != Fiber::eDead) {
          count++;
        }
      }
    }

    return Fixnum::from(count);
  }

  void Machine::vm_thread_fibers(STATE, Thread* thread,
      std::function<void (STATE, Fiber*)> f)
  {
    std::lock_guard<std::mutex> guard(thread_nexus()->threads_mutex());

    for(ThreadList::iterator i = thread_nexus()->threads()->begin();
        i != thread_nexus()->threads()->end();
        ++i)
    {
      if(ThreadState* thread_state = *i) {
        if(thread_state->kind() == ThreadState::eFiber
            && !thread_state->fiber()->nil_p()
            && thread_state->fiber()->status() != Fiber::eDead
            && thread_state->fiber()->thread() == thread) {
          f(state, thread_state->fiber());
        }
      }
    }
  }

  Array* Machine::vm_thread_fibers(STATE, Thread* thread) {
    Array* fibers = Array::create(state, 0);

    vm_thread_fibers(state, thread,
          [fibers](STATE, Fiber* fiber){ fibers->append(state, fiber); });

    return fibers;
  }

  void Machine::halt_console(STATE) {
    if(_console_) {
      delete _console_;
      _console_ = nullptr;
    }
  }

  void Machine::halt_profiler(STATE) {
    if(_profiler_) {
      delete _profiler_;
      _profiler_ = nullptr;
    }
  }

  void Machine::halt_debugger(STATE) {
    if(_debugger_) {
      delete _debugger_;
      _debugger_ = nullptr;
    }
  }

  void Machine::halt_compiler(STATE) {
    if(_compiler_) {
      delete _compiler_;
      _compiler_ = nullptr;
    }
  }

  void Machine::halt_c_api(STATE) {
    if(_c_api_) {
      delete _c_api_;
      _c_api_ = nullptr;
    }
  }

  void Machine::halt_codedb(STATE) {
    if(!G(coredb)->nil_p()) G(coredb)->close(state);

    if(_codedb_) {
      delete _codedb_;
      _codedb_ = nullptr;
    }
  }

  void Machine::halt_signals(STATE) {
    if(_signals_) {
      signals()->stop(state);
      delete _signals_;
      _signals_ = nullptr;
    }
  }

  void Machine::halt_collector(STATE) {
    collector()->stop(state);

    collector()->dispose(state);
    collector()->finish(state);

    if(_collector_) {
      delete _collector_;
      _collector_ = nullptr;
    }
  }

  void Machine::halt_memory(STATE) {
    if(_memory_) {
      delete _memory_;
      _memory_ = nullptr;
    }
  }

  void Machine::halt_thread_nexus(STATE) {
    if(_thread_nexus_) {
      delete _thread_nexus_;
      _thread_nexus_ = nullptr;
    }
  }

  void Machine::halt_diagnostics(STATE) {
    if(_diagnostics_) {
      delete _diagnostics_;
      _diagnostics_ = nullptr;
    }
  }

  void Machine::halt_configuration(STATE) {
    if(_configuration_) {
      delete _configuration_;
      _configuration_ = nullptr;
    }
  }

  void Machine::halt_environment(STATE) {
    if(_environment_) {
      delete _environment_;
      _environment_ = nullptr;
    }
  }

  void Machine::halt_logger(STATE) {
    logger::close();

    if(_logger_) {
      delete _logger_;
      _logger_ = nullptr;
    }
  }

  void Machine::halt_machine_state(STATE) {
    if(_machine_state_) {
      delete _machine_state_;
      _machine_state_ = nullptr;
    }
  }

  void Machine::halt_machine(STATE) {
    NativeMethod::cleanup_thread(state);

    new(&_waiting_mutex_) std::mutex;
    new(&_waiting_condition_) std::condition_variable;

    state->discard();
  }

  int Machine::halt(STATE, Object* exit_code) {
    int code = -1;

    if(Fixnum* fix = try_as<Fixnum>(exit_code)) {
      code = fix->to_native();
    }

    return halt(state, code);
  }

  int Machine::halt(int exit_code) {
    return halt(ThreadState::current(), exit_code);
  }

  int Machine::halt(STATE, int exit_code) {
    machine_state()->exit_code(exit_code);

    // TODO: Machine, ensure everything checkpoints or defers on halting
    state->managed_phase(state);

    machine_state()->set_halting();

    if(configuration()->log_lifetime.value) {
      logger::write("process: exit: %s %d %lld %fs %fs",
          environment()->pid().c_str(), machine_state()->exit_code(),
          diagnostics()->codedb_metrics()->load_count,
          timer::elapsed_seconds(diagnostics()->codedb_metrics()->load_ns),
          machine_state()->run_time());
    }

    halt_console(state);
    halt_profiler(state);
    halt_debugger(state);
    halt_compiler(state);
    halt_c_api(state);
    halt_signals(state);
    halt_collector(state);

    thread_nexus()->halt(state, state);

    halt_codedb(state);
    halt_memory(state);
    halt_diagnostics(state);
    halt_thread_nexus(state);
    halt_configuration(state);
    halt_environment(state);
    halt_logger(state);
    halt_machine_state(state);
    halt_machine(state);

    exit(exit_code);

    return 0;
  }

  void Machine::trace_objects(STATE, std::function<void (STATE, Object**)> f) {
    _console_->trace_objects(state, f);
  }
}
