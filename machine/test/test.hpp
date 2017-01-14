#ifndef RBX_TEST_TEST_HPP
#define RBX_TEST_TEST_HPP

#include "vm.hpp"
#include "state.hpp"
#include "call_frame.hpp"
#include "config_parser.hpp"
#include "machine/object_utils.hpp"
#include "memory.hpp"
#include "configuration.hpp"
#include "metrics.hpp"
#include "machine/detection.hpp"
#include "machine/memory/immix_marker.hpp"
#include "class/thread.hpp"

#include <cxxtest/TestSuite.h>

using namespace rubinius;

// Initial attempt at creating Mock objects that behave like Array but
// do not descend from it. Needed for testing cast_for_multi_block_arg.
//class RespondsToToaryReturnsNil : public Object {
//public:
//  static RespondsToToaryReturnsNil* create(STATE, native_int size) {
//    RespondsToToaryReturnsNil* ary = state->memory()->new_object<RespondsToToaryReturnsNil>(state, G(array));
//
//    return ary;
//  }
//
//  Object* respond_to(STATE, Symbol* name, Object* priv) {
//    printf("called respond_to\n");
//    return cTrue;
//  }
//
//  static void to_ary(STATE, Object* value) {
//  }
//};
//
//class RespondsToToaryReturnsNonarray : public Array {
//  Fixnum* to_ary(STATE, Object* value) {
//    return Fixnum::from(42);
//  }
//};

class VMTest {
public:
  SharedState* shared;
  State* state;
  ConfigParser* config_parser;
  Configuration config;


  void setup_call_frame(CallFrame* cf, StackVariables* scope, int size) {
    scope->initialize(cNil, cNil, Module::create(state), 0);

    cf->prepare(size);
    cf->stack_ptr_ = cf->stk - 1;
    cf->previous = NULL;
    cf->lexical_scope_ = nil<LexicalScope>();
    cf->dispatch_data = NULL;
    cf->compiled_code = nil<CompiledCode>();
    cf->flags = 0;
    cf->top_scope_ = NULL;
    cf->scope = scope;
    cf->arguments = NULL;
  }

  // TODO: Fix this
  void initialize_as_root(VM* vm) {
    vm->set_current_thread();

    Memory* om = new Memory(vm, vm->shared);
    vm->shared.om = om;

    vm->shared.set_initialized();
    vm->shared.set_root_vm(vm);

    vm->managed_phase(state);

    State state(vm);

    TypeInfo::auto_learn_fields(&state);

    vm->bootstrap_ontology(&state);

    // Setup the main Thread, which is wrapper of the main native thread
    // when the VM boots.
    Thread::create(&state, vm);
    vm->thread()->alive(&state, cTrue);
    vm->thread()->sleep(&state, cFalse);
  }

  void create() {
    config_parser = new ConfigParser;
    shared = new SharedState(0, config, *config_parser);
    VM* vm = shared->thread_nexus()->new_vm(shared);
    initialize_as_root(vm);
    state = new State(vm);
  }

  void destroy() {
    if(Memory* om = state->memory()) {
      if(memory::ImmixMarker* im = om->immix_marker()) {
        im->stop(state);
      }
    }

    VM::discard(state, state->vm());
    delete shared;
    delete state;
  }

  void setUp() {
    create();
  }

  void tearDown() {
    destroy();
  }
};

#endif
