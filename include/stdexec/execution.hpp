/*
 * Copyright (c) 2021-2022 NVIDIA Corporation
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include "__detail/__execution_fwd.hpp"

// include these after __execution_fwd.hpp
#include "__detail/__as_awaitable.hpp"
#include "__detail/__basic_sender.hpp"
#include "__detail/__completion_signatures.hpp"
#include "__detail/__connect_awaitable.hpp"
#include "__detail/__cpo.hpp"
#include "__detail/__debug.hpp"
#include "__detail/__domain.hpp"
#include "__detail/__env.hpp"
#include "__detail/__intrusive_ptr.hpp"
#include "__detail/__intrusive_slist.hpp"
#include "__detail/__meta.hpp"
#include "__detail/__operation_states.hpp"
#include "__detail/__receivers.hpp"
#include "__detail/__schedulers.hpp"
#include "__detail/__senders.hpp"
#include "__detail/__transform_sender.hpp"
#include "__detail/__transform_completion_signatures.hpp"
#include "__detail/__type_traits.hpp"
#include "__detail/__utility.hpp"
#include "__detail/__with_awaitable_senders.hpp"

#include "functional.hpp"
#include "concepts.hpp"
#include "coroutine.hpp"
#include "stop_token.hpp"

#include <atomic>
#include <cassert>
#include <concepts>
#include <condition_variable>
#include <stdexcept>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <variant>
#include <cstddef>
#include <exception>
#include <utility>

STDEXEC_PRAGMA_PUSH()
STDEXEC_PRAGMA_IGNORE_GNU("-Wundefined-inline")
STDEXEC_PRAGMA_IGNORE_GNU("-Wsubobject-linkage")
STDEXEC_PRAGMA_IGNORE_GNU("-Wmissing-braces")

STDEXEC_PRAGMA_IGNORE_EDG(1302)
STDEXEC_PRAGMA_IGNORE_EDG(497)
STDEXEC_PRAGMA_IGNORE_EDG(type_qualifiers_ignored_on_reference)

namespace stdexec {
  /////////////////////////////////////////////////////////////////////////////
  template <class _Sender, class _Scheduler, class _Tag = set_value_t>
  concept __completes_on =
    __decays_to<__call_result_t<get_completion_scheduler_t<_Tag>, env_of_t<_Sender>>, _Scheduler>;

  /////////////////////////////////////////////////////////////////////////////
  template <class _Sender, class _Scheduler, class _Env>
  concept __starts_on = __decays_to<__call_result_t<get_scheduler_t, _Env>, _Scheduler>;

  // NOT TO SPEC
  template <class _Tag, const auto& _Predicate>
  concept tag_category = //
    requires {
      typename __mbool<bool{_Predicate(_Tag{})}>;
      requires bool { _Predicate(_Tag{}) };
    };

  namespace {
    inline constexpr auto __ref = []<class _Ty>(_Ty& __ty) noexcept {
      return [__ty = &__ty]() noexcept -> decltype(auto) {
        return (*__ty);
      };
    };
  } // namespace

  template <class _Ty>
  using __ref_t = decltype(__ref(__declval<_Ty&>()));

  /////////////////////////////////////////////////////////////////////////////
  // NOT TO SPEC: __submit
  namespace __submit_ {
    template <class _OpRef>
    struct __receiver {
      using receiver_concept = receiver_t;
      using __t = __receiver;
      using __id = __receiver;

      using _Operation = __decay_t<__call_result_t<_OpRef>>;
      using _Receiver = stdexec::__t<__mapply<__q<__msecond>, _Operation>>;

      _OpRef __opref_;

      // Forward all the receiver ops, and delete the operation state.
      template <__completion_tag _Tag, class... _As>
        requires __callable<_Tag, _Receiver, _As...>
      friend void tag_invoke(_Tag __tag, __receiver&& __self, _As&&... __as) noexcept {
        __tag(static_cast<_Receiver&&>(__self.__opref_().__rcvr_), static_cast<_As&&>(__as)...);
        __self.__delete_op();
      }

      void __delete_op() noexcept {
        _Operation* __op = &__opref_();
        if constexpr (__callable<get_allocator_t, env_of_t<_Receiver>>) {
          auto&& __env = get_env(__op->__rcvr_);
          auto __alloc = get_allocator(__env);
          using _Alloc = decltype(__alloc);
          using _OpAlloc =
            typename std::allocator_traits<_Alloc>::template rebind_alloc<_Operation>;
          _OpAlloc __op_alloc{__alloc};
          std::allocator_traits<_OpAlloc>::destroy(__op_alloc, __op);
          std::allocator_traits<_OpAlloc>::deallocate(__op_alloc, __op, 1);
        } else {
          delete __op;
        }
      }

      // Forward all receiever queries.
      STDEXEC_MEMFN_DECL(
        auto get_env)(this const __receiver& __self) noexcept -> env_of_t<_Receiver&> {
        return get_env(__self.__opref_().__rcvr_);
      }
    };

    template <class _SenderId, class _ReceiverId>
    struct __operation {
      using _Sender = stdexec::__t<_SenderId>;
      using _Receiver = stdexec::__t<_ReceiverId>;
      using __receiver_t = __receiver<__ref_t<__operation>>;

      STDEXEC_ATTRIBUTE((no_unique_address))
      _Receiver __rcvr_;
      connect_result_t<_Sender, __receiver_t> __op_state_;

      __operation(_Sender&& __sndr, _Receiver __rcvr)
        : __rcvr_(static_cast<_Receiver&&>(__rcvr))
        , __op_state_(connect(static_cast<_Sender&&>(__sndr), __receiver_t{__ref(*this)})) {
      }
    };

    struct __submit_t {
      template <receiver _Receiver, sender_to<_Receiver> _Sender>
      void operator()(_Sender&& __sndr, _Receiver __rcvr) const noexcept(false) {
        if constexpr (__callable<get_allocator_t, env_of_t<_Receiver>>) {
          auto&& __env = get_env(__rcvr);
          auto __alloc = get_allocator(__env);
          using _Alloc = decltype(__alloc);
          using _Op = __operation<__id<_Sender>, __id<_Receiver>>;
          using _OpAlloc = typename std::allocator_traits<_Alloc>::template rebind_alloc<_Op>;
          _OpAlloc __op_alloc{__alloc};
          auto __op = std::allocator_traits<_OpAlloc>::allocate(__op_alloc, 1);
          try {
            std::allocator_traits<_OpAlloc>::construct(
              __op_alloc, __op, static_cast<_Sender&&>(__sndr), static_cast<_Receiver&&>(__rcvr));
            start(__op->__op_state_);
          } catch (...) {
            std::allocator_traits<_OpAlloc>::deallocate(__op_alloc, __op, 1);
            throw;
          }
        } else {
          start((new __operation<__id<_Sender>, __id<_Receiver>>{
                   static_cast<_Sender&&>(__sndr), static_cast<_Receiver&&>(__rcvr)})
                  ->__op_state_);
        }
      }
    };
  } // namespace __submit_

  using __submit_::__submit_t;
  inline constexpr __submit_t __submit{};

  namespace __inln {
    struct __schedule_t { };

    struct __scheduler {
      using __t = __scheduler;
      using __id = __scheduler;

      template <class _Tag = __schedule_t>
      STDEXEC_ATTRIBUTE((host, device))
      STDEXEC_MEMFN_DECL(
        auto schedule)(this __scheduler) {
        return __make_sexpr<_Tag>();
      }

      STDEXEC_MEMFN_DECL(auto query)(this __scheduler, get_forward_progress_guarantee_t) noexcept
        -> forward_progress_guarantee {
        return forward_progress_guarantee::weakly_parallel;
      }

      auto operator==(const __scheduler&) const noexcept -> bool = default;
    };
  } // namespace __inln

  template <>
  struct __sexpr_impl<__inln::__schedule_t> : __sexpr_defaults {
    static constexpr auto get_attrs = //
      [](__ignore) noexcept
      -> __env::__with<__inln::__scheduler, get_completion_scheduler_t<set_value_t>> {
      return __env::__with(__inln::__scheduler{}, get_completion_scheduler<set_value_t>);
    };

    static constexpr auto get_completion_signatures = //
      [](__ignore, __ignore) noexcept -> completion_signatures<set_value_t()> {
      return {};
    };

    static constexpr auto start = //
      []<class _Receiver>(__ignore, _Receiver& __rcvr) noexcept -> void {
      set_value(static_cast<_Receiver&&>(__rcvr));
    };
  };

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.consumer.start_detached]
  namespace __start_detached {
    template <class _EnvId>
    struct __detached_receiver {
      using _Env = stdexec::__t<_EnvId>;

      struct __t {
        using receiver_concept = receiver_t;
        using __id = __detached_receiver;
        STDEXEC_ATTRIBUTE((no_unique_address))
        _Env __env_;

        template <class... _As>
        STDEXEC_MEMFN_DECL(void set_value)(this __t&&, _As&&...) noexcept {
        }

        template <class _Error>
        [[noreturn]] STDEXEC_MEMFN_DECL(
          void set_error)(this __t&&, _Error&&) noexcept {
          std::terminate();
        }

        template <same_as<set_stopped_t> _Tag>
        friend void tag_invoke(_Tag, __t&&) noexcept {
        }

        STDEXEC_MEMFN_DECL(auto get_env)(this const __t& __self) noexcept -> const _Env& {
          // BUGBUG NOT TO SPEC
          return __self.__env_;
        }
      };
    };
    template <class _Env = empty_env>
    using __detached_receiver_t = __t<__detached_receiver<__id<__decay_t<_Env>>>>;

    struct start_detached_t {
      template <sender_in<__root_env_t> _Sender>
        requires __callable<apply_sender_t, __early_domain_of_t<_Sender>, start_detached_t, _Sender>
      void operator()(_Sender&& __sndr) const {
        auto __domain = __get_early_domain(__sndr);
        stdexec::apply_sender(__domain, *this, static_cast<_Sender&&>(__sndr));
      }

      template <class _Env, sender_in<__as_root_env_t<_Env>> _Sender>
        requires __callable<
          apply_sender_t,
          __late_domain_of_t<_Sender, __as_root_env_t<_Env>>,
          start_detached_t,
          _Sender,
          __as_root_env_t<_Env>>
      void operator()(_Sender&& __sndr, _Env&& __env) const {
        auto __domain = __get_late_domain(__sndr, __env);
        stdexec::apply_sender(
          __domain,
          *this,
          static_cast<_Sender&&>(__sndr),
          __as_root_env(static_cast<_Env&&>(__env)));
      }

      using _Sender = __0;
      using __legacy_customizations_t = __types<
        tag_invoke_t(
          start_detached_t,
          get_completion_scheduler_t<set_value_t>(get_env_t(const _Sender&)),
          _Sender),
        tag_invoke_t(start_detached_t, _Sender)>;

      template <class _Sender, class _Env = __root_env_t>
        requires sender_to<_Sender, __detached_receiver_t<_Env>>
      void apply_sender(_Sender&& __sndr, _Env&& __env = {}) const {
        __submit(
          static_cast<_Sender&&>(__sndr), __detached_receiver_t<_Env>{static_cast<_Env&&>(__env)});
      }
    };
  } // namespace __start_detached

  using __start_detached::start_detached_t;
  inline constexpr start_detached_t start_detached{};

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.factories]
  namespace __just {
    template <class _JustTag>
    struct __impl : __sexpr_defaults {
      using __tag_t = typename _JustTag::__tag_t;

      static constexpr auto get_completion_signatures =
        []<class _Sender>(_Sender&&, __ignore) noexcept {
          static_assert(sender_expr_for<_Sender, _JustTag>);
          return completion_signatures<__mapply<__qf<__tag_t>, __decay_t<__data_of<_Sender>>>>{};
        };

      static constexpr auto start =
        []<class _State, class _Receiver>(_State& __state, _Receiver& __rcvr) noexcept -> void {
        __tup::__apply(
          [&]<class... _Ts>(_Ts&... __ts) noexcept {
            __tag_t()(std::move(__rcvr), std::move(__ts)...);
          },
          __state);
      };
    };

    struct just_t {
      using __tag_t = set_value_t;

      template <__movable_value... _Ts>
      STDEXEC_ATTRIBUTE((host, device))
      auto
        operator()(_Ts&&... __ts) const noexcept((__nothrow_decay_copyable<_Ts> && ...)) {
        return __make_sexpr<just_t>(__tuple{static_cast<_Ts&&>(__ts)...});
      }
    };

    struct just_error_t {
      using __tag_t = set_error_t;

      template <__movable_value _Error>
      STDEXEC_ATTRIBUTE((host, device))
      auto
        operator()(_Error&& __err) const noexcept(__nothrow_decay_copyable<_Error>) {
        return __make_sexpr<just_error_t>(__tuple{static_cast<_Error&&>(__err)});
      }
    };

    struct just_stopped_t {
      using __tag_t = set_stopped_t;

      template <class _Tag = just_stopped_t>
      STDEXEC_ATTRIBUTE((host, device))
      auto
        operator()() const noexcept {
        return __make_sexpr<_Tag>(__tuple{});
      }
    };
  } // namespace __just

  using __just::just_t;
  using __just::just_error_t;
  using __just::just_stopped_t;

  template <>
  struct __sexpr_impl<just_t> : __just::__impl<just_t> { };

  template <>
  struct __sexpr_impl<just_error_t> : __just::__impl<just_error_t> { };

  template <>
  struct __sexpr_impl<just_stopped_t> : __just::__impl<just_stopped_t> { };

  inline constexpr just_t just{};
  inline constexpr just_error_t just_error{};
  inline constexpr just_stopped_t just_stopped{};

  /////////////////////////////////////////////////////////////////////////////
  // [execution.execute]
  namespace __execute_ {
    template <class _Fun>
    struct __as_receiver {
      using receiver_concept = receiver_t;
      _Fun __fun_;

      template <same_as<set_value_t> _Tag>
      friend void tag_invoke(_Tag, __as_receiver&& __rcvr) noexcept {
        try {
          __rcvr.__fun_();
        } catch (...) {
          set_error(static_cast<__as_receiver&&>(__rcvr), std::exception_ptr());
        }
      }

      template <same_as<set_error_t> _Tag>
      [[noreturn]]
      friend void tag_invoke(_Tag, __as_receiver&&, std::exception_ptr) noexcept {
        std::terminate();
      }

      template <same_as<set_stopped_t> _Tag>
      friend void tag_invoke(_Tag, __as_receiver&&) noexcept {
      }

      STDEXEC_MEMFN_DECL(auto get_env)(this const __as_receiver&) noexcept -> empty_env {
        return {};
      }
    };

    struct execute_t {
      template <scheduler _Scheduler, class _Fun>
        requires __callable<_Fun&> && move_constructible<_Fun>
      void operator()(_Scheduler&& __sched, _Fun __fun) const noexcept(false) {
        // Look for a legacy customization
        if constexpr (tag_invocable<execute_t, _Scheduler, _Fun>) {
          tag_invoke(execute_t{}, static_cast<_Scheduler&&>(__sched), static_cast<_Fun&&>(__fun));
        } else {
          auto __domain = query_or(get_domain, __sched, default_domain());
          stdexec::apply_sender(
            __domain,
            *this,
            schedule(static_cast<_Scheduler&&>(__sched)),
            static_cast<_Fun&&>(__fun));
        }
      }

      template <sender_of<set_value_t()> _Sender, class _Fun>
        requires __callable<_Fun&> && move_constructible<_Fun>
      void apply_sender(_Sender&& __sndr, _Fun __fun) const noexcept(false) {
        __submit(static_cast<_Sender&&>(__sndr), __as_receiver<_Fun>{static_cast<_Fun&&>(__fun)});
      }
    };
  } // namespace __execute_

  using __execute_::execute_t;
  inline constexpr execute_t execute{};

  // NOT TO SPEC:
  namespace __closure {
    template <__class _Dp>
    struct sender_adaptor_closure;
  } // namespace __closure

  using __closure::sender_adaptor_closure;

  template <class _Tp>
  concept __sender_adaptor_closure =
    derived_from<__decay_t<_Tp>, sender_adaptor_closure<__decay_t<_Tp>>>
    && move_constructible<__decay_t<_Tp>> && constructible_from<__decay_t<_Tp>, _Tp>;

  template <class _Tp, class _Sender>
  concept __sender_adaptor_closure_for = __sender_adaptor_closure<_Tp> && sender<__decay_t<_Sender>>
                                      && __callable<_Tp, __decay_t<_Sender>>
                                      && sender<__call_result_t<_Tp, __decay_t<_Sender>>>;

  namespace __closure {
    template <class _T0, class _T1>
    struct __compose : sender_adaptor_closure<__compose<_T0, _T1>> {
      STDEXEC_ATTRIBUTE((no_unique_address))
      _T0 __t0_;
      STDEXEC_ATTRIBUTE((no_unique_address))
      _T1 __t1_;

      template <sender _Sender>
        requires __callable<_T0, _Sender> && __callable<_T1, __call_result_t<_T0, _Sender>>
      STDEXEC_ATTRIBUTE((always_inline))
      __call_result_t<_T1, __call_result_t<_T0, _Sender>>
        operator()(_Sender&& __sndr) && {
        return static_cast<_T1&&>(__t1_)(static_cast<_T0&&>(__t0_)(static_cast<_Sender&&>(__sndr)));
      }

      template <sender _Sender>
        requires __callable<const _T0&, _Sender>
              && __callable<const _T1&, __call_result_t<const _T0&, _Sender>>
      STDEXEC_ATTRIBUTE((always_inline))
      __call_result_t<_T1, __call_result_t<_T0, _Sender>>
        operator()(_Sender&& __sndr) const & {
        return __t1_(__t0_(static_cast<_Sender&&>(__sndr)));
      }
    };

    template <__class _Dp>
    struct sender_adaptor_closure { };

    template <sender _Sender, __sender_adaptor_closure_for<_Sender> _Closure>
    STDEXEC_ATTRIBUTE((always_inline))
    __call_result_t<_Closure, _Sender>
      operator|(_Sender&& __sndr, _Closure&& __clsur) {
      return static_cast<_Closure&&>(__clsur)(static_cast<_Sender&&>(__sndr));
    }

    template <__sender_adaptor_closure _T0, __sender_adaptor_closure _T1>
    STDEXEC_ATTRIBUTE((always_inline))
    __compose<__decay_t<_T0>, __decay_t<_T1>>
      operator|(_T0&& __t0, _T1&& __t1) {
      return {{}, static_cast<_T0&&>(__t0), static_cast<_T1&&>(__t1)};
    }

    template <class _Fun, class... _As>
    struct __binder_back
      : __tup::__tuple_for<_As...>
      , sender_adaptor_closure<__binder_back<_Fun, _As...>> {
      STDEXEC_ATTRIBUTE((no_unique_address))
      _Fun __fun_{};

      template <sender _Sender>
        requires __callable<_Fun, _Sender, _As...>
      STDEXEC_ATTRIBUTE((host, device, always_inline))
      __call_result_t<_Fun, _Sender, _As...>
        operator()(_Sender&& __sndr) && noexcept(__nothrow_callable<_Fun, _Sender, _As...>) {
        return __tup::__apply(
          [&__sndr, this](_As&... __as) noexcept(__nothrow_callable<_Fun, _Sender, _As...>)
            -> __call_result_t<_Fun, _Sender, _As...> {
            return static_cast<_Fun&&>(__fun_)(
              static_cast<_Sender&&>(__sndr), static_cast<_As&&>(__as)...);
          },
          *this);
      }

      template <sender _Sender>
        requires __callable<const _Fun&, _Sender, const _As&...>
      STDEXEC_ATTRIBUTE((host, device, always_inline))
      auto
        operator()(_Sender&& __sndr) const & //
        noexcept(__nothrow_callable<const _Fun&, _Sender, const _As&...>)
          -> __call_result_t<const _Fun&, _Sender, const _As&...> {
        return __tup::__apply(
          [&__sndr,
           this](const _As&... __as) noexcept(__nothrow_callable<_Fun, _Sender, const _As&...>)
            -> __call_result_t<const _Fun&, _Sender, const _As&...> {
            return __fun_(static_cast<_Sender&&>(__sndr), __as...);
          },
          *this);
      }
    };
  } // namespace __closure

  using __closure::__binder_back;

  namespace __adaptors {
    namespace __no {
      struct __nope { };

      struct __receiver : __nope {
        using receiver_concept = receiver_t;
      };

      template <same_as<set_error_t> _Tag>
      void tag_invoke(_Tag, __receiver, std::exception_ptr) noexcept;
      template <same_as<set_stopped_t> _Tag>
      void tag_invoke(_Tag, __receiver) noexcept;
      auto tag_invoke(get_env_t, __receiver) noexcept -> empty_env;
    } // namespace __no

    using __not_a_receiver = __no::__receiver;

    template <class _Base>
    struct __adaptor_base {
      template <class _T1>
        requires constructible_from<_Base, _T1>
      explicit __adaptor_base(_T1&& __base)
        : __base_(static_cast<_T1&&>(__base)) {
      }

     private:
      STDEXEC_ATTRIBUTE((no_unique_address))
      _Base __base_;

     protected:
      STDEXEC_ATTRIBUTE((host, device, always_inline))

      _Base&
        base() & noexcept {
        return __base_;
      }

      STDEXEC_ATTRIBUTE((host, device, always_inline))

      const _Base&
        base() const & noexcept {
        return __base_;
      }

      STDEXEC_ATTRIBUTE((host, device, always_inline))

      _Base&&
        base() && noexcept {
        return static_cast<_Base&&>(__base_);
      }
    };

    template <derived_from<__no::__nope> _Base>
    struct __adaptor_base<_Base> { };

// BUGBUG Not to spec: on gcc and nvc++, member functions in derived classes
// don't shadow type aliases of the same name in base classes. :-O
// On mingw gcc, 'bool(type::existing_member_function)' evaluates to true,
// but 'int(type::existing_member_function)' is an error (as desired).
#define STDEXEC_DISPATCH_MEMBER(_TAG)                                                              \
  template <class _Self, class... _Ts>                                                             \
  STDEXEC_ATTRIBUTE((host, device, always_inline))                                                 \
  static auto __call_##_TAG(_Self&& __self, _Ts&&... __ts) noexcept                                \
    -> decltype((static_cast<_Self&&>(__self))._TAG(static_cast<_Ts&&>(__ts)...)) {                \
    static_assert(noexcept((static_cast<_Self&&>(__self))._TAG(static_cast<_Ts&&>(__ts)...)));     \
    return static_cast<_Self&&>(__self)._TAG(static_cast<_Ts&&>(__ts)...);                         \
  } /**/
#define STDEXEC_CALL_MEMBER(_TAG, ...) __call_##_TAG(__VA_ARGS__)

#if STDEXEC_CLANG()
// Only clang gets this right.
#  define STDEXEC_MISSING_MEMBER(_Dp, _TAG) requires { typename _Dp::_TAG; }
#  define STDEXEC_DEFINE_MEMBER(_TAG)       STDEXEC_DISPATCH_MEMBER(_TAG) using _TAG = void
#else
#  define STDEXEC_MISSING_MEMBER(_Dp, _TAG) (__missing_##_TAG<_Dp>())
#  define STDEXEC_DEFINE_MEMBER(_TAG)                                                              \
    template <class _Dp>                                                                           \
    static constexpr bool __missing_##_TAG() noexcept {                                            \
      return requires { requires bool(int(_Dp::_TAG)); };                                          \
    }                                                                                              \
    STDEXEC_DISPATCH_MEMBER(_TAG)                                                                  \
    static constexpr int _TAG = 1 /**/
#endif

    template <__class _Derived, class _Base = __not_a_receiver>
    struct receiver_adaptor
      : __adaptor_base<_Base>
      , receiver_t {
      friend _Derived;
      STDEXEC_DEFINE_MEMBER(set_value);
      STDEXEC_DEFINE_MEMBER(set_error);
      STDEXEC_DEFINE_MEMBER(set_stopped);
      STDEXEC_DEFINE_MEMBER(get_env);

      static constexpr bool __has_base = !derived_from<_Base, __no::__nope>;

      template <class _Dp>
      using __base_from_derived_t = decltype(__declval<_Dp>().base());

      using __get_base_t =
        __if_c<__has_base, __mbind_back_q<__copy_cvref_t, _Base>, __q<__base_from_derived_t>>;

      template <class _Dp>
      using __base_t = __minvoke<__get_base_t, _Dp&&>;

      template <class _Dp>
      STDEXEC_ATTRIBUTE((host, device))
      static auto
        __get_base(_Dp&& __self) noexcept -> __base_t<_Dp> {
        if constexpr (__has_base) {
          return __c_upcast<receiver_adaptor>(static_cast<_Dp&&>(__self)).base();
        } else {
          return static_cast<_Dp&&>(__self).base();
        }
      }

      template <same_as<set_value_t> _SetValue, class... _As>
      STDEXEC_ATTRIBUTE((host, device, always_inline))
      friend auto
        tag_invoke(_SetValue, _Derived&& __self, _As&&... __as) noexcept //
        -> __msecond<                                                    //
          __if_c<same_as<set_value_t, _SetValue>>,
          decltype(STDEXEC_CALL_MEMBER(
            set_value,
            static_cast<_Derived&&>(__self),
            static_cast<_As&&>(__as)...))> {
        static_assert(noexcept(STDEXEC_CALL_MEMBER(
          set_value, static_cast<_Derived&&>(__self), static_cast<_As&&>(__as)...)));
        STDEXEC_CALL_MEMBER(set_value, static_cast<_Derived&&>(__self), static_cast<_As&&>(__as)...);
      }

      template <same_as<set_value_t> _SetValue, class _Dp = _Derived, class... _As>
        requires STDEXEC_MISSING_MEMBER(_Dp, set_value) && tag_invocable<_SetValue, __base_t<_Dp>, _As...>
      STDEXEC_ATTRIBUTE((host, device, always_inline))
      friend void
        tag_invoke(_SetValue, _Derived&& __self, _As&&... __as) noexcept {
        stdexec::set_value(__get_base(static_cast<_Dp&&>(__self)), static_cast<_As&&>(__as)...);
      }

      template <same_as<set_error_t> _SetError, class _Error>
      STDEXEC_ATTRIBUTE((host, device, always_inline))
      friend auto
        tag_invoke(_SetError, _Derived&& __self, _Error&& __err) noexcept //
        -> __msecond<                                                     //
          __if_c<same_as<set_error_t, _SetError>>,
          decltype(STDEXEC_CALL_MEMBER(
            set_error,
            static_cast<_Derived&&>(__self),
            static_cast<_Error&&>(__err)))> {
        static_assert(noexcept(STDEXEC_CALL_MEMBER(
          set_error, static_cast<_Derived&&>(__self), static_cast<_Error&&>(__err))));
        STDEXEC_CALL_MEMBER(
          set_error, static_cast<_Derived&&>(__self), static_cast<_Error&&>(__err));
      }

      template <same_as<set_error_t> _SetError, class _Error, class _Dp = _Derived>
        requires STDEXEC_MISSING_MEMBER(_Dp, set_error) && tag_invocable<_SetError, __base_t<_Dp>, _Error>
      STDEXEC_ATTRIBUTE((host, device, always_inline))
      friend void
        tag_invoke(_SetError, _Derived&& __self, _Error&& __err) noexcept {
        stdexec::set_error(
          __get_base(static_cast<_Derived&&>(__self)), static_cast<_Error&&>(__err));
      }

      template <same_as<set_stopped_t> _SetStopped, class _Dp = _Derived>
      STDEXEC_ATTRIBUTE((host, device, always_inline))
      friend auto
        tag_invoke(_SetStopped, _Derived&& __self) noexcept //
        -> __msecond<                                       //
          __if_c<same_as<set_stopped_t, _SetStopped>>,
          decltype(STDEXEC_CALL_MEMBER(set_stopped, static_cast<_Dp&&>(__self)))> {
        static_assert(noexcept(STDEXEC_CALL_MEMBER(set_stopped, static_cast<_Derived&&>(__self))));
        STDEXEC_CALL_MEMBER(set_stopped, static_cast<_Derived&&>(__self));
      }

      template <same_as<set_stopped_t> _SetStopped, class _Dp = _Derived>
        requires STDEXEC_MISSING_MEMBER(_Dp, set_stopped) && tag_invocable<_SetStopped, __base_t<_Dp>>
      STDEXEC_ATTRIBUTE((host, device, always_inline))
      friend void
        tag_invoke(_SetStopped, _Derived&& __self) noexcept {
        stdexec::set_stopped(__get_base(static_cast<_Derived&&>(__self)));
      }

      // Pass through the get_env receiver query
      template <same_as<get_env_t> _GetEnv, class _Dp = _Derived>
      STDEXEC_ATTRIBUTE((host, device, always_inline))
      friend auto
        tag_invoke(_GetEnv, const _Derived& __self) noexcept
        -> decltype(STDEXEC_CALL_MEMBER(get_env, static_cast<const _Dp&>(__self))) {
        static_assert(noexcept(STDEXEC_CALL_MEMBER(get_env, __self)));
        return STDEXEC_CALL_MEMBER(get_env, __self);
      }

      template <same_as<get_env_t> _GetEnv, class _Dp = _Derived>
        requires STDEXEC_MISSING_MEMBER(_Dp, get_env)
          STDEXEC_ATTRIBUTE((host, device, always_inline))
      friend auto
        tag_invoke(_GetEnv, const _Derived& __self) noexcept -> env_of_t<__base_t<const _Dp&>> {
        return stdexec::get_env(__get_base(__self));
      }

     public:
      receiver_adaptor() = default;
      using __adaptor_base<_Base>::__adaptor_base;

      using receiver_concept = receiver_t;
    };
  } // namespace __adaptors

  template <__class _Derived, receiver _Base = __adaptors::__not_a_receiver>
  using receiver_adaptor = __adaptors::receiver_adaptor<_Derived, _Base>;

  template <class _Receiver, class _Fun, class... _As>
  concept __receiver_of_invoke_result = //
    receiver_of<
      _Receiver,
      completion_signatures<
        __minvoke<__remove<void, __qf<set_value_t>>, __invoke_result_t<_Fun, _As...>>>>;

  template <bool _CanThrow = false, class _Receiver, class _Fun, class... _As>
  void __set_value_invoke(_Receiver&& __rcvr, _Fun&& __fun, _As&&... __as) noexcept(!_CanThrow) {
    if constexpr (_CanThrow || __nothrow_invocable<_Fun, _As...>) {
      if constexpr (same_as<void, __invoke_result_t<_Fun, _As...>>) {
        __invoke(static_cast<_Fun&&>(__fun), static_cast<_As&&>(__as)...);
        set_value(static_cast<_Receiver&&>(__rcvr));
      } else {
        set_value(
          static_cast<_Receiver&&>(__rcvr),
          __invoke(static_cast<_Fun&&>(__fun), static_cast<_As&&>(__as)...));
      }
    } else {
      try {
        stdexec::__set_value_invoke<true>(
          static_cast<_Receiver&&>(__rcvr),
          static_cast<_Fun&&>(__fun),
          static_cast<_As&&>(__as)...);
      } catch (...) {
        set_error(static_cast<_Receiver&&>(__rcvr), std::current_exception());
      }
    }
  }

  template <class _Fun>
  struct _WITH_FUNCTION_ { };

  template <class... _Args>
  struct _WITH_ARGUMENTS_ { };

  inline constexpr __mstring __not_callable_diag =
    "The specified function is not callable with the arguments provided."_mstr;

  template <__mstring _Context, __mstring _Diagnostic = __not_callable_diag>
  struct _NOT_CALLABLE_ { };

  template <__mstring _Context>
  struct __callable_error {
    template <class _Fun, class... _Args>
    using __f =     //
      __mexception< //
        _NOT_CALLABLE_<_Context>,
        _WITH_FUNCTION_<_Fun>,
        _WITH_ARGUMENTS_<_Args...>>;
  };

  template <class _Fun, class... _Args>
    requires __invocable<_Fun, _Args...>
  using __non_throwing_ = __mbool<__nothrow_invocable<_Fun, _Args...>>;

  template <class _Tag, class _Fun, class _Sender, class _Env, class _Catch>
  using __with_error_invoke_t = //
    __if<
      __gather_completions_for<
        _Tag,
        _Sender,
        _Env,
        __mbind_front<__mtry_catch_q<__non_throwing_, _Catch>, _Fun>,
        __q<__mand>>,
      completion_signatures<>,
      __with_exception_ptr>;

  template <class _Fun, class... _Args>
    requires __invocable<_Fun, _Args...>
  using __set_value_invoke_t = //
    completion_signatures<
      __minvoke<__remove<void, __qf<set_value_t>>, __invoke_result_t<_Fun, _Args...>>>;

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.then]
  namespace __then {
    inline constexpr __mstring __then_context = "In stdexec::then(Sender, Function)..."_mstr;
    using __on_not_callable = __callable_error<__then_context>;

    template <class _Fun, class _CvrefSender, class _Env>
    using __completion_signatures_t = //
      __try_make_completion_signatures<
        _CvrefSender,
        _Env,
        __with_error_invoke_t<set_value_t, _Fun, _CvrefSender, _Env, __on_not_callable>,
        __mbind_front<__mtry_catch_q<__set_value_invoke_t, __on_not_callable>, _Fun>>;

    ////////////////////////////////////////////////////////////////////////////////////////////////
    struct then_t {
      template <sender _Sender, __movable_value _Fun>
      auto operator()(_Sender&& __sndr, _Fun __fun) const -> __well_formed_sender auto {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<then_t>(static_cast<_Fun&&>(__fun), static_cast<_Sender&&>(__sndr)));
      }

      template <__movable_value _Fun>
      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()(_Fun __fun) const -> __binder_back<then_t, _Fun> {
        return {{static_cast<_Fun&&>(__fun)}};
      }

      using _Sender = __1;
      using _Fun = __0;
      using __legacy_customizations_t = __types<
        tag_invoke_t(
          then_t,
          get_completion_scheduler_t<set_value_t>(get_env_t(_Sender&)),
          _Sender,
          _Fun),
        tag_invoke_t(then_t, _Sender, _Fun)>;
    };

    struct __then_impl : __sexpr_defaults {
      static constexpr auto get_completion_signatures = //
        []<class _Sender, class _Env>(_Sender&&, _Env&&) noexcept
        -> __completion_signatures_t<__decay_t<__data_of<_Sender>>, __child_of<_Sender>, _Env> {
        static_assert(sender_expr_for<_Sender, then_t>);
        return {};
      };

      static constexpr auto complete = //
        []<class _Tag, class... _Args>(
          __ignore,
          auto& __state,
          auto& __rcvr,
          _Tag,
          _Args&&... __args) noexcept -> void {
        if constexpr (std::same_as<_Tag, set_value_t>) {
          stdexec::__set_value_invoke(
            std::move(__rcvr), std::move(__state), static_cast<_Args&&>(__args)...);
        } else {
          _Tag()(std::move(__rcvr), static_cast<_Args&&>(__args)...);
        }
      };
    };
  } // namespace __then

  using __then::then_t;

  /// @brief The then sender adaptor, which invokes a function with the result of
  ///        a sender, making the result available to the next receiver.
  /// @hideinitializer
  inline constexpr then_t then{};

  template <>
  struct __sexpr_impl<then_t> : __then::__then_impl { };

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.upon_error]
  namespace __upon_error {
    inline constexpr __mstring __upon_error_context =
      "In stdexec::upon_error(Sender, Function)..."_mstr;
    using __on_not_callable = __callable_error<__upon_error_context>;

    template <class _Fun, class _CvrefSender, class _Env>
    using __completion_signatures_t = //
      __try_make_completion_signatures<
        _CvrefSender,
        _Env,
        __with_error_invoke_t<set_error_t, _Fun, _CvrefSender, _Env, __on_not_callable>,
        __q<__compl_sigs::__default_set_value>,
        __mbind_front<__mtry_catch_q<__set_value_invoke_t, __on_not_callable>, _Fun>>;

    ////////////////////////////////////////////////////////////////////////////////////////////////
    struct upon_error_t {
      template <sender _Sender, __movable_value _Fun>
      auto operator()(_Sender&& __sndr, _Fun __fun) const -> __well_formed_sender auto {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<upon_error_t>(static_cast<_Fun&&>(__fun), static_cast<_Sender&&>(__sndr)));
      }

      template <__movable_value _Fun>
      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()(_Fun __fun) const -> __binder_back<upon_error_t, _Fun> {
        return {{static_cast<_Fun&&>(__fun)}};
      }

      using _Sender = __1;
      using _Fun = __0;
      using __legacy_customizations_t = __types<
        tag_invoke_t(
          upon_error_t,
          get_completion_scheduler_t<set_value_t>(get_env_t(_Sender&)),
          _Sender,
          _Fun),
        tag_invoke_t(upon_error_t, _Sender, _Fun)>;
    };

    struct __upon_error_impl : __sexpr_defaults {
      static constexpr auto get_completion_signatures = //
        []<class _Sender, class _Env>(_Sender&&, _Env&&) noexcept
        -> __completion_signatures_t<__decay_t<__data_of<_Sender>>, __child_of<_Sender>, _Env> {
        static_assert(sender_expr_for<_Sender, upon_error_t>);
        return {};
      };

      static constexpr auto complete = //
        []<class _Tag, class... _Args>(
          __ignore,
          auto& __state,
          auto& __rcvr,
          _Tag,
          _Args&&... __args) noexcept -> void {
        if constexpr (std::same_as<_Tag, set_error_t>) {
          stdexec::__set_value_invoke(
            std::move(__rcvr), std::move(__state), static_cast<_Args&&>(__args)...);
        } else {
          _Tag()(std::move(__rcvr), static_cast<_Args&&>(__args)...);
        }
      };
    };
  } // namespace __upon_error

  using __upon_error::upon_error_t;
  inline constexpr upon_error_t upon_error{};

  template <>
  struct __sexpr_impl<upon_error_t> : __upon_error::__upon_error_impl { };

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.upon_stopped]
  namespace __upon_stopped {
    inline constexpr __mstring __upon_stopped_context =
      "In stdexec::upon_stopped(Sender, Function)..."_mstr;
    using __on_not_callable = __callable_error<__upon_stopped_context>;

    template <class _Fun, class _CvrefSender, class _Env>
    using __completion_signatures_t = //
      __try_make_completion_signatures<
        _CvrefSender,
        _Env,
        __with_error_invoke_t<set_stopped_t, _Fun, _CvrefSender, _Env, __on_not_callable>,
        __q<__compl_sigs::__default_set_value>,
        __q<__compl_sigs::__default_set_error>,
        __set_value_invoke_t<_Fun>>;

    ////////////////////////////////////////////////////////////////////////////////////////////////
    struct upon_stopped_t {
      template <sender _Sender, __movable_value _Fun>
        requires __callable<_Fun>
      auto operator()(_Sender&& __sndr, _Fun __fun) const -> __well_formed_sender auto {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<upon_stopped_t>(static_cast<_Fun&&>(__fun), static_cast<_Sender&&>(__sndr)));
      }

      template <__movable_value _Fun>
        requires __callable<_Fun>
      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()(_Fun __fun) const -> __binder_back<upon_stopped_t, _Fun> {
        return {{static_cast<_Fun&&>(__fun)}};
      }

      using _Sender = __1;
      using _Fun = __0;
      using __legacy_customizations_t = __types<
        tag_invoke_t(
          upon_stopped_t,
          get_completion_scheduler_t<set_value_t>(get_env_t(_Sender&)),
          _Sender,
          _Fun),
        tag_invoke_t(upon_stopped_t, _Sender, _Fun)>;
    };

    struct __upon_stopped_impl : __sexpr_defaults {
      static constexpr auto get_completion_signatures = //
        []<class _Sender, class _Env>(_Sender&&, _Env&&) noexcept
        -> __completion_signatures_t<__decay_t<__data_of<_Sender>>, __child_of<_Sender>, _Env> {
        static_assert(sender_expr_for<_Sender, upon_stopped_t>);
        return {};
      };

      static constexpr auto complete = //
        []<class _Tag, class... _Args>(
          __ignore,
          auto& __state,
          auto& __rcvr,
          _Tag,
          _Args&&... __args) noexcept -> void {
        if constexpr (std::same_as<_Tag, set_stopped_t>) {
          stdexec::__set_value_invoke(
            std::move(__rcvr), std::move(__state), static_cast<_Args&&>(__args)...);
        } else {
          _Tag()(std::move(__rcvr), static_cast<_Args&&>(__args)...);
        }
      };
    };
  } // namespace __upon_stopped

  using __upon_stopped::upon_stopped_t;
  inline constexpr upon_stopped_t upon_stopped{};

  template <>
  struct __sexpr_impl<upon_stopped_t> : __upon_stopped::__upon_stopped_impl { };

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.bulk]
  namespace __bulk {
    inline constexpr __mstring __bulk_context = "In stdexec::bulk(Sender, Shape, Function)..."_mstr;
    using __on_not_callable = __callable_error<__bulk_context>;

    template <class _Shape, class _Fun>
    struct __data {
      _Shape __shape_;
      STDEXEC_ATTRIBUTE((no_unique_address))
      _Fun __fun_;
      static constexpr auto __mbrs_ = __mliterals<&__data::__shape_, &__data::__fun_>();
    };
    template <class _Shape, class _Fun>
    __data(_Shape, _Fun) -> __data<_Shape, _Fun>;

    template <class _Ty>
    using __decay_ref = __decay_t<_Ty>&;

    template <class _CvrefSender, class _Env, class _Shape, class _Fun, class _Catch>
    using __with_error_invoke_t = //
      __if<
        __try_value_types_of_t<
          _CvrefSender,
          _Env,
          __transform<
            __q<__decay_ref>,
            __mbind_front<__mtry_catch_q<__non_throwing_, _Catch>, _Fun, _Shape>>,
          __q<__mand>>,
        completion_signatures<>,
        __with_exception_ptr>;

    template <class _CvrefSender, class _Env, class _Shape, class _Fun>
    using __completion_signatures = //
      __try_make_completion_signatures<
        _CvrefSender,
        _Env,
        __with_error_invoke_t<_CvrefSender, _Env, _Shape, _Fun, __on_not_callable>>;

    struct bulk_t {
      template <sender _Sender, integral _Shape, __movable_value _Fun>
      STDEXEC_ATTRIBUTE((host, device))
      auto
        operator()(_Sender&& __sndr, _Shape __shape, _Fun __fun) const -> __well_formed_sender
        auto {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<bulk_t>(
            __data{__shape, static_cast<_Fun&&>(__fun)}, static_cast<_Sender&&>(__sndr)));
      }

      template <integral _Shape, class _Fun>
      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()(_Shape __shape, _Fun __fun) const -> __binder_back<bulk_t, _Shape, _Fun> {
        return {
          {static_cast<_Shape&&>(__shape), static_cast<_Fun&&>(__fun)}
        };
      }

      // This describes how to use the pieces of a bulk sender to find
      // legacy customizations of the bulk algorithm.
      using _Sender = __1;
      using _Shape = __nth_member<0>(__0);
      using _Fun = __nth_member<1>(__0);
      using __legacy_customizations_t = __types<
        tag_invoke_t(
          bulk_t,
          get_completion_scheduler_t<set_value_t>(get_env_t(_Sender&)),
          _Sender,
          _Shape,
          _Fun),
        tag_invoke_t(bulk_t, _Sender, _Shape, _Fun)>;
    };

    struct __bulk_impl : __sexpr_defaults {
      template <class _Sender>
      using __fun_t = decltype(__decay_t<__data_of<_Sender>>::__fun_);

      template <class _Sender>
      using __shape_t = decltype(__decay_t<__data_of<_Sender>>::__shape_);

      static constexpr auto get_completion_signatures = //
        []<class _Sender, class _Env>(_Sender&&, _Env&&) noexcept
        -> __completion_signatures<__child_of<_Sender>, _Env, __shape_t<_Sender>, __fun_t<_Sender>> {
        static_assert(sender_expr_for<_Sender, bulk_t>);
        return {};
      };

      static constexpr auto complete = //
        []<class _Tag, class... _Args>(
          __ignore,
          auto& __state,
          auto& __rcvr,
          _Tag,
          _Args&&... __args) noexcept -> void {
        if constexpr (std::same_as<_Tag, set_value_t>) {
          using __shape_t = decltype(__state.__shape_);
          if constexpr (noexcept(__state.__fun_(__shape_t{}, __args...))) {
            for (__shape_t __i{}; __i != __state.__shape_; ++__i) {
              __state.__fun_(__i, __args...);
            }
            _Tag()(std::move(__rcvr), static_cast<_Args&&>(__args)...);
          } else {
            try {
              for (__shape_t __i{}; __i != __state.__shape_; ++__i) {
                __state.__fun_(__i, __args...);
              }
              _Tag()(std::move(__rcvr), static_cast<_Args&&>(__args)...);
            } catch (...) {
              set_error(std::move(__rcvr), std::current_exception());
            }
          }
        } else {
          _Tag()(std::move(__rcvr), static_cast<_Args&&>(__args)...);
        }
      };
    };
  } // namespace __bulk

  using __bulk::bulk_t;
  inline constexpr bulk_t bulk{};

  template <>
  struct __sexpr_impl<bulk_t> : __bulk::__bulk_impl { };

  ////////////////////////////////////////////////////////////////////////////
  // shared components of split and ensure_started
  //
  // The split and ensure_started algorithms are very similar in implementation.
  // The salient differences are:
  //
  // split: the input async operation is always connected. It is only
  //   started when one of the split senders is connected and started.
  //   split senders are copyable, so there are multiple operation states
  //   to be notified on completion. These are stored in an instrusive
  //   linked list.
  //
  // ensure_started: the input async operation is always started, so
  //   the internal receiver will always be completed. The ensure_started
  //   sender is move-only and single-shot, so there will only ever be one
  //   operation state to be notified on completion.
  //
  // The shared state should add-ref itself when the input async
  // operation is started and release itself when its completion
  // is notified.
  namespace __shared {
    template <class _BaseEnv>
    using __env_t = //
      __env::__join_t<
        __env::__with<inplace_stop_token, get_stop_token_t>,
        _BaseEnv>; // BUGBUG NOT TO SPEC

    template <class _Receiver>
    auto __make_notify_visitor(_Receiver& __rcvr) noexcept {
      return [&]<class _Tuple>(_Tuple&& __tupl) noexcept -> void {
        __apply(
          [&](auto __tag, auto&&... __args) noexcept -> void {
            __tag(static_cast<_Receiver&&>(__rcvr), __forward_like<_Tuple>(__args)...);
          },
          __tupl);
      };
    }

    struct __local_state_base : __immovable {
      using __notify_fn = void(__local_state_base*) noexcept;

      __notify_fn* __notify_{};
      __local_state_base* __next_{};
    };

    template <class _CvrefSender, class _Env>
    struct __shared_state;

    // The operation state of ensure_started, and each operation state of split, has one of these,
    // created when the sender is connected. There are 0 or more of them for each underlying async
    // operation. It is what ensure_started- and split-sender's `get_state` fn returns. It holds a
    // ref count to the shared state.
    template <class _CvrefSender, class _Receiver>
    struct __local_state
      : __local_state_base
      , __enable_receiver_from_this<_CvrefSender, _Receiver> {
      using __tag_t = tag_of_t<_CvrefSender>;
      using __stok_t = stop_token_of_t<env_of_t<_Receiver>>;
      static_assert(__one_of<__tag_t, __split::__split_t, __ensure_started::__ensure_started_t>);

      explicit __local_state(_CvrefSender&& __sndr) noexcept
        : __local_state::__local_state_base{{}, &__notify<tag_of_t<_CvrefSender>>}
        , __sh_state_(__get_sh_state(__sndr)) {
      }

      ~__local_state() {
        __sh_state_t::__detach(__sh_state_);
      }

      // Stop request callback:
      void operator()() noexcept {
        // We reach here when a split/ensure_started sender has received a stop request from the
        // receiver to which it is connected.
        if (std::unique_lock __lock{__sh_state_->__mutex_}) {
          // Remove this operation from the waiters list. Removal can fail if:
          //   1. It was already removed by another thread, or
          //   2. It hasn't been added yet (see `start` below), or
          //   3. The underlying operation has already completed.
          //
          // In each case, the right thing to do is nothing. If (1) then we raced with another
          // thread and lost. In that case, the other thread will take care of it. If (2) then
          // `start` will take care of it. If (3) then this stop request is safe to ignore.
          if (!__sh_state_->__waiters_.remove(this))
            return;
        }

        // The following code and the __notify function cannot both execute. This is because the
        // __notify function is called from the shared state's __notify_waiters function, which
        // first sets __waiters_ to the completed state. As a result, the attempt to remove `this`
        // from the waiters list above will fail and this stop request is ignored.
        __sh_state_t::__detach(__sh_state_);
        stdexec::set_stopped(static_cast<_Receiver&&>(this->__receiver()));
      }

      // This is called from __shared_state::__notify_waiters when the input async operation
      // completes; or, if it has already completed when start is called, it is called from start:
      // __notify cannot race with __on_stop_request. See comment in __on_stop_request.
      template <class _Tag>
      static void __notify(__local_state_base* __base) noexcept {
        auto* const __self = static_cast<__local_state*>(__base);

        // The split algorithm sends by T const&. ensure_started sends by T&&.
        constexpr bool __is_split = same_as<__split::__split_t, _Tag>;
        using __variant_t = decltype(__self->__sh_state_->__results_);
        using __cv_variant_t = __if_c<__is_split, const __variant_t&, __variant_t>;

        __self->__on_stop_.reset();

        auto __visitor = __make_notify_visitor(__self->__receiver());
        std::visit(__visitor, static_cast<__cv_variant_t&&>(__self->__sh_state_->__results_));
      }

      static auto __get_sh_state(_CvrefSender& __sndr) noexcept {
        return __sndr.apply(static_cast<_CvrefSender&&>(__sndr), __detail::__get_data()).__sh_state_;
      }

      using __sh_state_ptr_t = __result_of<__get_sh_state, _CvrefSender&>;
      using __sh_state_t = typename __sh_state_ptr_t::element_type;

      std::optional<stop_callback_for_t<__stok_t, __local_state&>> __on_stop_{};
      __sh_state_ptr_t __sh_state_;
    };

    template <class _CvrefSenderId, class _EnvId>
    struct __receiver {
      using _CvrefSender = stdexec::__cvref_t<_CvrefSenderId>;
      using _Env = stdexec::__t<_EnvId>;

      struct __t {
        using receiver_concept = receiver_t;
        using __id = __receiver;

        template <class... _As>
        STDEXEC_ATTRIBUTE((always_inline))
        STDEXEC_MEMFN_DECL(
          void set_value)(this __t&& __self, _As&&... __as) noexcept {
          __self.__sh_state_->__complete(set_value_t(), static_cast<_As&&>(__as)...);
        }

        template <class _Error>
        STDEXEC_ATTRIBUTE((always_inline))
        STDEXEC_MEMFN_DECL(
          void set_error)(this __t&& __self, _Error&& __err) noexcept {
          __self.__sh_state_->__complete(set_error_t(), static_cast<_Error&&>(__err));
        }

        STDEXEC_ATTRIBUTE((always_inline))
        STDEXEC_MEMFN_DECL(
          void set_stopped)(this __t&& __self) noexcept {
          __self.__sh_state_->__complete(set_stopped_t());
        }

        STDEXEC_MEMFN_DECL(auto get_env)(this const __t& __self) noexcept -> const __env_t<_Env>& {
          return __self.__sh_state_->__env_;
        }

        // The receiver does not hold a reference to the shared state.
        __shared_state<_CvrefSender, _Env>* __sh_state_;
      };
    };

    inline __local_state_base* __get_tombstone() noexcept {
      static __local_state_base __tombstone_{{}, nullptr, nullptr};
      return &__tombstone_;
    }

    template <class _CvrefSender, class _Env>
    struct __shared_state
      : private __enable_intrusive_from_this<__shared_state<_CvrefSender, _Env>, 2> {
      using __receiver_t = __t<__receiver<__cvref_id<_CvrefSender>, __id<_Env>>>;
      using __waiters_list_t = __intrusive_slist<&__local_state_base::__next_>;
      using __variant_t = __compl_sigs::__for_all_sigs<
        __completion_signatures_of_t<_CvrefSender, _Env>,
        __q<__decayed_tuple>,
        __mbind_front_q<
          __variant,
          std::tuple<set_stopped_t>, // Initial state of the variant is set_stopped
          std::tuple<set_error_t, std::exception_ptr>>>;

      static constexpr std::size_t __started_bit = 0;
      static constexpr std::size_t __completed_bit = 1;

      inplace_stop_source __stop_source_{};
      __env_t<_Env> __env_;
      __variant_t __results_{}; // Defaults to the "set_stopped" state
      std::mutex __mutex_;      // This mutex guards access to __waiters_.
      __waiters_list_t __waiters_{};
      connect_result_t<_CvrefSender, __receiver_t> __shared_op_;

      explicit __shared_state(_CvrefSender&& __sndr, _Env __env)
        : __env_(__env::__join(
          __env::__with(__stop_source_.get_token(), get_stop_token),
          static_cast<_Env&&>(__env)))
        , __shared_op_(connect(static_cast<_CvrefSender&&>(__sndr), __receiver_t{this})) {
        // add one ref count to account for the case where there are no watchers left but the
        // shared op is still running.
        this->__inc_ref();
      }

      // The caller of this wants to release their reference to the shared state. The ref
      // count must be at least 2 at this point: one owned by the caller, and one added in the
      // __shared_state ctor.
      static void __detach(__intrusive_ptr<__shared_state, 2>& __ptr) noexcept {
        // Ask the intrusive ptr to stop managing the reference count so we can manage it manually.
        if (auto* __self = __ptr.__release_()) {
          auto __old = __self->__dec_ref();
          STDEXEC_ASSERT(__count(__old) >= 2);

          if (__count(__old) == 2) {
            // The last watcher has released its reference. Asked the shared op to stop.
            static_cast<__shared_state*>(__self)->__stop_source_.request_stop();

            // Additionally, if the shared op was never started, or if it has already completed,
            // then the shared state is no longer needed. Decrement the ref count to 0 here, which
            // will delete __self.
            if (!__bit<__started_bit>(__old) || __bit<__completed_bit>(__old)) {
              __self->__dec_ref();
            }
          }
        }
      }

      /// @post The started bit is set in the shared state's ref count, OR the __waiters_ list
      /// is set to the known "tombstone" value indicating completion.
      void __try_start() noexcept {
        // With the split algorithm, multiple split senders can be started simultaneously, but
        // only one should start the shared async operation. If the "started" bit is set, then
        // someone else has already started the shared operation. Do nothing.
        if (this->template __is_set<__started_bit>()) {
          return;
        } else if (__bit<__started_bit>(this->template __set_bit<__started_bit>())) {
          return;
        } else if (__stop_source_.stop_requested()) {
          // Stop has already been requested. Rather than starting the operation, complete with
          // set_stopped immediately.
          // 1. Sets __waiters_ to a known "tombstone" value
          // 2. Notifies all the waiters that the operation has stopped
          // 3. Sets the "completed" bit in the ref count.
          __notify_waiters();
          return;
        } else {
          stdexec::start(__shared_op_);
        }
      }

      template <class _StopToken>
      bool __try_add_waiter(__local_state_base* __waiter, _StopToken __stok) noexcept {
        std::unique_lock __lock{__mutex_};
        if (__waiters_.front() == __get_tombstone()) {
          // The work has already completed. Notify the waiter immediately.
          __lock.unlock();
          __waiter->__notify_(__waiter);
          return true;
        } else if (__stok.stop_requested()) {
          // Stop has been requested. Do not add the waiter.
          return false;
        } else {
          // Add the waiter to the list.
          __waiters_.push_front(__waiter);
          return true;
        }
      }

      /// @brief This is called when the shared async operation completes.
      /// @post __waiters_ is set to a known "tombstone" value.
      template <class _Tag, class... _As>
      void __complete(_Tag, _As&&... __as) noexcept {
        try {
          using __tuple_t = __decayed_tuple<_Tag, _As...>;
          __results_.template emplace<__tuple_t>(_Tag(), static_cast<_As&&>(__as)...);
        } catch (...) {
          using __tuple_t = __decayed_tuple<set_error_t, std::exception_ptr>;
          __results_.template emplace<__tuple_t>(set_error, std::current_exception());
        }

        __notify_waiters();
      }

      /// @brief This is called when the shared async operation completes.
      /// @post __waiters_ is set to a known "tombstone" value.
      void __notify_waiters() noexcept {
        __waiters_list_t __waiters_copy{__get_tombstone()};

        // Set the waiters list to a known "tombstone" value that we can check later.
        {
          std::lock_guard __lock{__mutex_};
          __waiters_.swap(__waiters_copy);
        }

        STDEXEC_ASSERT(__waiters_copy.front() != __get_tombstone());
        for (__local_state_base* __item: __waiters_copy) {
          __item->__notify_(__item);
        }

        // Set the "completed" bit in the ref count. If the ref count is 1, then there are no more
        // waiters. Release the final reference.
        if (__count(this->template __set_bit<__completed_bit>()) == 1) {
          this->__dec_ref(); // release the extra ref count, deletes this
        }
      }
    };

    template <class _Cvref, class _CvrefSender, class _Env>
    using __make_completions = //
      __try_make_completion_signatures<
        // NOT TO SPEC:
        // See https://github.com/cplusplus/sender-receiver/issues/23
        _CvrefSender,
        __env_t<_Env>,
        completion_signatures<
          set_error_t(__minvoke<_Cvref, std::exception_ptr>),
          set_stopped_t()>, // NOT TO SPEC
        __transform<_Cvref, __mcompose<__q<completion_signatures>, __qf<set_value_t>>>,
        __transform<_Cvref, __mcompose<__q<completion_signatures>, __qf<set_error_t>>>>;

    // split completes with const T&. ensure_started completes with T&&.
    template <class _Tag>
    using __cvref_results_t = //
      __mcompose<__if_c<same_as<_Tag, __split::__split_t>, __cpclr, __cp>, __q<__decay_t>>;

    // NOTE: the use of __mapply in the return type below takes advantage of the fact that _ShState
    // denotes an instance of the __shared_state template, which is parameterized on the
    // cvref-qualified sender and the environment.
    template <class _Tag, class _ShState>
    using __completions = //
      __mapply<__mbind_front_q<__make_completions, __cvref_results_t<_Tag>>, _ShState>;

    template <class _CvrefSender, class _Env, bool _Copyable = true>
    struct __box {
      using __tag_t = __if_c<_Copyable, __split::__split_t, __ensure_started::__ensure_started_t>;
      using __sh_state_t = __shared_state<_CvrefSender, _Env>;

      __box(__tag_t, __intrusive_ptr<__sh_state_t, 2> __sh_state) noexcept
        : __sh_state_(std::move(__sh_state)) {
      }

      __box(__box&&) noexcept = default;
      __box(const __box&) noexcept
        requires _Copyable
      = default;

      ~__box() {
        __sh_state_t::__detach(__sh_state_);
      }

      __intrusive_ptr<__sh_state_t, 2> __sh_state_;
    };

    template <class _CvrefSender, class _Env>
    __box(__split::__split_t, __intrusive_ptr<__shared_state<_CvrefSender, _Env>, 2>) //
      ->__box<_CvrefSender, _Env, true>;

    template <class _CvrefSender, class _Env>
    __box(
      __ensure_started::__ensure_started_t,
      __intrusive_ptr<__shared_state<_CvrefSender, _Env>, 2>) -> __box<_CvrefSender, _Env, false>;

    template <class _Tag>
    struct __shared_impl : __sexpr_defaults {
      static constexpr auto get_state = //
        []<class _CvrefSender, class _Receiver>(_CvrefSender&& __sndr, _Receiver&) noexcept
        -> __local_state<_CvrefSender, _Receiver> {
        static_assert(sender_expr_for<_CvrefSender, _Tag>);
        return __local_state<_CvrefSender, _Receiver>{static_cast<_CvrefSender&&>(__sndr)};
      };

      static constexpr auto get_completion_signatures = //
        []<class _Self, class _OtherEnv>(const _Self&, _OtherEnv&&) noexcept
        -> __completions<_Tag, typename __data_of<_Self>::__sh_state_t> {
        static_assert(sender_expr_for<_Self, _Tag>);
        return {};
      };

      static constexpr auto start = //
        []<class _Sender, class _Receiver>(
          __local_state<_Sender, _Receiver>& __self,
          _Receiver& __rcvr) noexcept -> void {
        using __sh_state_t = typename __local_state<_Sender, _Receiver>::__sh_state_t;
        // Scenario: there are no more split senders, this is the only operation state, the
        // underlying operation has not yet been started, and the receiver's stop token is already
        // in the "stop requested" state. Then registering the stop callback will call
        // __on_stop_request on __self synchronously. It may also be called asynchronously at
        // any point after the callback is registered. Beware. We are guaranteed, however, that
        // __on_stop_request will not complete the operation or decrement the shared state's ref
        // count until after __self has been added to the waiters list.
        const auto __stok = stdexec::get_stop_token(stdexec::get_env(__rcvr));
        __self.__on_stop_.emplace(__stok, __self);

        // We haven't put __self in the waiters list yet and we are holding a ref count to
        // __sh_state_, so nothing can happen to the __sh_state_ here.

        // Start the shared op. As an optimization, skip it if the receiver's stop token has already
        // been signaled.
        if (!__stok.stop_requested()) {
          __self.__sh_state_->__try_start();
          if (__self.__sh_state_->__try_add_waiter(&__self, __stok)) {
            // successfully added the waiter
            return;
          }
        }

        // Otherwise, failed to add the waiter because of a stop-request.
        // Complete synchronously with set_stopped().
        __self.__on_stop_.reset();
        __sh_state_t::__detach(__self.__sh_state_);
        stdexec::set_stopped(static_cast<_Receiver&&>(__rcvr));
      };
    };
  } // namespace __shared

  ////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.split]
  namespace __split {
    using namespace __shared;

    struct __split_t { };

    struct split_t {
      template <sender _Sender, class _Env = empty_env>
        requires sender_in<_Sender, _Env> && __decay_copyable<env_of_t<_Sender>>
      auto operator()(_Sender&& __sndr, _Env&& __env = {}) const -> __well_formed_sender auto {
        auto __domain = __get_late_domain(__sndr, __env);
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<split_t>(static_cast<_Env&&>(__env), static_cast<_Sender&&>(__sndr)));
      }

      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()() const noexcept -> __binder_back<split_t> {
        return {};
      }

      using _Sender = __1;
      using __legacy_customizations_t = //
        __types<
          tag_invoke_t(
            split_t,
            get_completion_scheduler_t<set_value_t>(get_env_t(const _Sender&)),
            _Sender),
          tag_invoke_t(split_t, _Sender)>;

      template <class _CvrefSender, class _Env>
      using __receiver_t = __t<__meval<__receiver, __cvref_id<_CvrefSender>, __id<_Env>>>;

      template <class _Sender>
      static auto transform_sender(_Sender&& __sndr) {
        using _Receiver = __receiver_t<__child_of<_Sender>, __decay_t<__data_of<_Sender>>>;
        static_assert(sender_to<__child_of<_Sender>, _Receiver>);
        return __sexpr_apply(
          static_cast<_Sender&&>(__sndr),
          [&]<class _Env, class _Child>(__ignore, _Env&& __env, _Child&& __child) {
            // The shared state starts life with a ref-count of one.
            auto __sh_state = __make_intrusive<__shared_state<_Child, __decay_t<_Env>>, 2>(
              static_cast<_Child&&>(__child), static_cast<_Env&&>(__env));
            return __make_sexpr<__split_t>(__box{__split_t(), std::move(__sh_state)});
          });
      }
    };
  } // namespace __split

  using __split::split_t;
  inline constexpr split_t split{};

  template <>
  struct __sexpr_impl<__split::__split_t> : __shared::__shared_impl<__split::__split_t> { };

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.ensure_started]
  namespace __ensure_started {
    using namespace __shared;

    struct __ensure_started_t { };

    struct ensure_started_t {
      template <sender _Sender, class _Env = empty_env>
        requires sender_in<_Sender, _Env> && __decay_copyable<env_of_t<_Sender>>
      [[nodiscard]]
      auto
        operator()(_Sender&& __sndr, _Env&& __env = {}) const -> __well_formed_sender auto {
        if constexpr (sender_expr_for<_Sender, __ensure_started_t>) {
          return static_cast<_Sender&&>(__sndr);
        } else {
          auto __domain = __get_late_domain(__sndr, __env);
          return stdexec::transform_sender(
            __domain,
            __make_sexpr<ensure_started_t>(
              static_cast<_Env&&>(__env), static_cast<_Sender&&>(__sndr)));
        }
      }

      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()() const noexcept -> __binder_back<ensure_started_t> {
        return {};
      }

      using _Sender = __1;
      using __legacy_customizations_t = //
        __types<
          tag_invoke_t(
            ensure_started_t,
            get_completion_scheduler_t<set_value_t>(get_env_t(const _Sender&)),
            _Sender),
          tag_invoke_t(ensure_started_t, _Sender)>;

      template <class _CvrefSender, class _Env>
      using __receiver_t = __t<__meval<__receiver, __cvref_id<_CvrefSender>, __id<_Env>>>;

      template <class _Sender>
      static auto transform_sender(_Sender&& __sndr) {
        using _Receiver = __receiver_t<__child_of<_Sender>, __decay_t<__data_of<_Sender>>>;
        static_assert(sender_to<__child_of<_Sender>, _Receiver>);
        return __sexpr_apply(
          static_cast<_Sender&&>(__sndr),
          [&]<class _Env, class _Child>(__ignore, _Env&& __env, _Child&& __child) {
            // The shared state starts life with a ref-count of one.
            auto __sh_state = __make_intrusive<__shared_state<_Child, __decay_t<_Env>>, 2>(
              static_cast<_Child&&>(__child), static_cast<_Env&&>(__env));
            // Eagerly start the work:
            __sh_state->__try_start();
            return __make_sexpr<__ensure_started_t>(
              __box{__ensure_started_t(), std::move(__sh_state)});
          });
      }
    };
  } // namespace __ensure_started

  using __ensure_started::ensure_started_t;
  inline constexpr ensure_started_t ensure_started{};

  template <>
  struct __sexpr_impl<__ensure_started::__ensure_started_t>
    : __shared::__shared_impl<__ensure_started::__ensure_started_t> { };

  STDEXEC_PRAGMA_PUSH()
  STDEXEC_PRAGMA_IGNORE_EDG(not_used_in_partial_spec_arg_list)

  /////////////////////////////////////////////////////////////////////////////
  // a receiver adaptor that augments its environment
  namespace __detail {
    template <auto _ReceiverPtr, auto... _EnvFns>
    struct __receiver_with;

    template <class _Operation, class _Receiver, _Receiver _Operation::*_ReceiverPtr, auto... _EnvFns>
    struct __receiver_with<_ReceiverPtr, _EnvFns...> {
      struct __t : receiver_adaptor<__t> {
        using __id = __receiver_with;
        using __env_t = __env::__join_t<__result_of<_EnvFns, _Operation*>..., env_of_t<_Receiver>>;

        _Operation* __op_state_;

        auto base() && noexcept -> _Receiver&& {
          return static_cast<_Receiver&&>(__op_state_->*_ReceiverPtr);
        }

        auto get_env() const noexcept -> __env_t {
          return __env::__join(
            _EnvFns(__op_state_)..., stdexec::get_env(__op_state_->*_ReceiverPtr));
        }
      };
    };
  } // namespace __detail

  STDEXEC_PRAGMA_POP()

  //////////////////////////////////////////////////////////////////////////////
  // [exec.let]

  namespace __any_ {
    template <class _Sig>
    struct __rcvr_vfun;

    template <class _Tag, class... _Args>
    struct __rcvr_vfun<_Tag(_Args...)> {
      void (*__complete_)(void*, _Args&&...) noexcept;

      void operator()(void* __rcvr, _Tag, _Args&&... __args) const noexcept {
        __complete_(__rcvr, std::forward<_Args>(__args)...);
      }
    };

    template <class _Receiver, class _Tag, class... _Args>
    constexpr auto __rcvr_vfun_fn(_Tag (*)(_Args...)) noexcept {
      return +[](void* __pointer, _Args&&... __args) noexcept {
        _Receiver* __rcvr = static_cast<_Receiver*>(__pointer);
        _Tag{}(static_cast<_Receiver&&>(*__rcvr), static_cast<_Args&&>(__args)...);
      };
    }
  } // namespace __any_

  namespace __let {
    // FUTURE: when we have a scheduler query for "always completes inline",
    // then we can use that instead of hard-coding `__inln::__scheduler` here.
    template <class _Scheduler>
    concept __unknown_context = __one_of<_Scheduler, __none_such, __inln::__scheduler>;

    template <class _Sigs, class _Env>
    struct __receiver_vtable_for;

    template <class _OpState, class _Tag, class... _Args>
    constexpr auto __rcvr_vfun_from_op_state_fn(_Tag (*)(_Args...)) noexcept {
      return +[](void* __pointer, _Args&&... __args) noexcept {
        auto* __op_state = static_cast<_OpState*>(__pointer);
        _Tag{}(std::move(__op_state->__rcvr_), static_cast<_Args&&>(__args)...);
      };
    }

    template <class _Env, class... _Sigs>
    struct __receiver_vtable_for<completion_signatures<_Sigs...>, _Env>
      : stdexec::__any_::__rcvr_vfun<_Sigs>... {

      _Env (*__do_get_env)(const void* __rcvr) noexcept;

      template <class _OpState>
      explicit constexpr __receiver_vtable_for(const _OpState*) noexcept
        : stdexec::__any_::__rcvr_vfun<_Sigs>{__rcvr_vfun_from_op_state_fn<_OpState>(
          static_cast<_Sigs*>(nullptr))}...
        , __do_get_env{+[](const void* __pointer) noexcept -> _Env {
          auto* __op_state = static_cast<const _OpState*>(__pointer);
          auto& __state = __op_state->__state_;
          const auto& __rcvr = __op_state->__rcvr_;
          auto __env = get_env(__rcvr);
          auto __sched = __state.__sched_;
          if constexpr (__unknown_context<decltype(__sched)>) {
            return __env;
          } else {
            return __env::__join(
              __env::__with(__sched, get_scheduler),
              __env::__without(std::move(__env), get_domain));
          }
        }} {
      }

      using stdexec::__any_::__rcvr_vfun<_Sigs>::operator()...;

      auto __get_env(const void* __rcvr) const noexcept -> _Env {
        return __do_get_env(__rcvr);
      }
    };

    template <class _OpState, class _Env, class _Sigs>
    inline constexpr __receiver_vtable_for<_Sigs, _Env> __receiver_vtable_for_v{
      static_cast<const _OpState*>(nullptr)};

    template <class _Sigs, class _Env>
    class __receiver_ref {
     public:
      using receiver_concept = receiver_t;

      template <class _OpState>
      __receiver_ref(_OpState& __op_state) noexcept
        : __vtable_{&__receiver_vtable_for_v<_OpState, _Env, _Sigs>}
        , __state_{&__op_state} {
      }

      template <same_as<get_env_t> _Tag>
      friend auto tag_invoke(_Tag, const __receiver_ref& __self) noexcept {
        return __self.__vtable_->__get_env(__self.__state_);
      }

      template <__completion_tag _Tag, same_as<__receiver_ref> _Self, class... _As>
      friend void tag_invoke(_Tag, _Self&& __self, _As&&... __as) noexcept {
        (*__self.__vtable_)(__self.__state_, _Tag(), static_cast<_As&&>(__as)...);
      }

     private:
      const __receiver_vtable_for<_Sigs, _Env>* __vtable_;
      void* __state_;
    };

    template <class _Set, class _Domain = dependent_domain>
    struct __let_t;

    template <class _Set>
    inline constexpr __mstring __in_which_let_msg{"In stdexec::let_value(Sender, Function)..."};

    template <>
    inline constexpr __mstring __in_which_let_msg<set_error_t>{
      "In stdexec::let_error(Sender, Function)..."};

    template <>
    inline constexpr __mstring __in_which_let_msg<set_stopped_t>{
      "In stdexec::let_stopped(Sender, Function)..."};

    template <class _Set>
    using __on_not_callable = __callable_error<__in_which_let_msg<_Set>>;

    template <class _Receiver, class _Scheduler>
    struct __receiver_with_sched {
      using receiver_concept = receiver_t;
      _Receiver __rcvr_;
      _Scheduler __sched_;

      template <__completion_tag _Tag, same_as<__receiver_with_sched> _Self, class... _As>
      friend void tag_invoke(_Tag, _Self&& __self, _As&&... __as) noexcept {
        _Tag()(static_cast<_Receiver&&>(__self.__rcvr_), static_cast<_As&&>(__as)...);
      }

      template <same_as<get_env_t> _Tag>
      friend auto tag_invoke(_Tag, const __receiver_with_sched& __self) noexcept {
        return __env::__join(
          __env::__with(__self.__sched_, get_scheduler),
          __env::__without(get_env(__self.__rcvr_), get_domain));
      }
    };

    template <class _Receiver, class _Scheduler>
    __receiver_with_sched(_Receiver, _Scheduler) -> __receiver_with_sched<_Receiver, _Scheduler>;

    // If the input sender knows its completion scheduler, make it the current scheduler
    // in the environment seen by the result sender.
    template <class _Env, class _Scheduler>
    using __result_env_t = __if_c<
      __unknown_context<_Scheduler>,
      _Env,
      __env::
        __join_t<__env::__with<_Scheduler, get_scheduler_t>, __env::__without_t<_Env, get_domain_t>>>;

    template <class _Tp>
    using __decay_ref = __decay_t<_Tp>&;

    template <__mstring _Where, __mstring _What>
    struct _FUNCTION_MUST_RETURN_A_VALID_SENDER_IN_THE_CURRENT_ENVIRONMENT_ { };

#if STDEXEC_NVHPC()
    template <class _Sender, class _Env, class _Set>
    struct __bad_result_sender_ {
      using __t = __mexception<
        _FUNCTION_MUST_RETURN_A_VALID_SENDER_IN_THE_CURRENT_ENVIRONMENT_<
          __in_which_let_msg<_Set>,
          "The function must return a valid sender for the current environment"_mstr>,
        _WITH_SENDER_<_Sender>,
        _WITH_ENVIRONMENT_<_Env>>;
    };
    template <class _Sender, class _Env, class _Set>
    using __bad_result_sender = __t<__bad_result_sender_<_Sender, _Env, _Set>>;
#else
    template <class _Sender, class _Env, class _Set>
    using __bad_result_sender = __mexception<
      _FUNCTION_MUST_RETURN_A_VALID_SENDER_IN_THE_CURRENT_ENVIRONMENT_<
        __in_which_let_msg<_Set>,
        "The function must return a valid sender for the current environment"_mstr>,
      _WITH_SENDER_<_Sender>,
      _WITH_ENVIRONMENT_<_Env>>;
#endif

    template <class _Sender, class _Env, class _Set>
    using __ensure_sender = //
      __minvoke_if_c<
        sender_in<_Sender, _Env>,
        __q<__midentity>,
        __mbind_back_q<__bad_result_sender, _Env, _Set>,
        _Sender>;

    // A metafunction that computes the result sender type for a given set of argument types
    template <class _Fun, class _Set, class _Env, class _Sched>
    using __result_sender_fn = //
      __mcompose<
        __mbind_back_q<__ensure_sender, __result_env_t<_Env, _Sched>, _Set>,
        __transform<
          __q<__decay_ref>,
          __mbind_front<__mtry_catch_q<__call_result_t, __on_not_callable<_Set>>, _Fun>>>;

    // The receiver that gets connected to the result sender is the input receiver,
    // possibly augmented with the input sender's completion scheduler (which is
    // where the result sender will be started).
    template <class _Receiver, class _Scheduler>
    using __result_receiver_t =
      __if_c<__unknown_context<_Scheduler>, _Receiver, __receiver_with_sched<_Receiver, _Scheduler>>;

    template <class _ResultSender, class _Receiver, class _Scheduler>
    using __receiver_ref_t = //
      __receiver_ref<
        completion_signatures_of_t<_ResultSender, __result_env_t<env_of_t<_Receiver>, _Scheduler>>,
        __result_env_t<env_of_t<_Receiver>, _Scheduler>>;

    template <class _ResultSender, class _Receiver, class _Scheduler>
    concept __need_receiver_ref =
      __nothrow_connectable<_ResultSender, __receiver_ref_t<_ResultSender, _Receiver, _Scheduler>>
      && !__nothrow_connectable<_ResultSender, __result_receiver_t<_Receiver, _Scheduler>>;

    template <class _ResultSender, class _Env, class _Scheduler>
    concept __nothrow_connectable_receiver_ref =
      __nothrow_connectable<_ResultSender&&, __receiver_ref_t<_ResultSender, _Env, _Scheduler>>;

    template <class _ResultSender, class _Receiver, class _Scheduler>
    using __checked_result_receiver_t = //
      __if_c<
        __need_receiver_ref<_ResultSender, _Receiver, _Scheduler>,
        __receiver_ref_t<_ResultSender, _Receiver, _Scheduler>,
        __result_receiver_t<_Receiver, _Scheduler>>;

    template <class _ResultSender, class _Receiver, class _Scheduler>
    using __op_state_t = connect_result_t<
      _ResultSender,
      __checked_result_receiver_t<_ResultSender, _Receiver, _Scheduler>>;

    template <class _Receiver, class _Fun, class _Set, class _Sched>
    using __op_state_for = //
      __mcompose<
        __mbind_back_q<__op_state_t, _Receiver, _Sched>,
        __result_sender_fn<_Fun, _Set, env_of_t<_Receiver>, _Sched>>;

    template <class _Set, class _Sig>
    struct __tfx_signal_fn {
      template <class, class, class>
      using __f = completion_signatures<_Sig>;
    };

    template <class _Set, class... _Args>
    struct __tfx_signal_fn<_Set, _Set(_Args...)> {
      template <class _Env, class _Fun, class _Sched>
      using __f = //
        __try_make_completion_signatures<
          __minvoke<__result_sender_fn<_Fun, _Set, _Env, _Sched>, _Args...>,
          __result_env_t<_Env, _Sched>,
          __if_c<
            (__nothrow_decay_copyable<_Args> && ...) && __nothrow_callable<_Fun, _Args...>
              && __nothrow_connectable_receiver_ref<
                __minvoke<__result_sender_fn<_Fun, _Set, _Env, _Sched>, _Args...>,
                _Env,
                _Sched>,
            completion_signatures<>,
            completion_signatures<set_error_t(std::exception_ptr)>>>;
    };

    // `_Sched` is the input sender's completion scheduler, or __none_such if it doesn't have one.
    template <class _Env, class _Fun, class _Set, class _Sched, class _Sig>
    using __tfx_signal_t = __minvoke<__tfx_signal_fn<_Set, _Sig>, _Env, _Fun, _Sched>;

    template <class _Sender, class _Set>
    using __completion_sched =
      __query_result_or_t<get_completion_scheduler_t<_Set>, env_of_t<_Sender>, __none_such>;

    template <class _CvrefSender, class _Env, class _LetTag, class _Fun>
    using __completions = //
      __mapply<
        __transform<
          __mbind_front_q<
            __tfx_signal_t,
            _Env,
            _Fun,
            __t<_LetTag>,
            __completion_sched<_CvrefSender, __t<_LetTag>>>,
          __q<__concat_completion_signatures_t>>,
        __completion_signatures_of_t<_CvrefSender, _Env>>;

    template <__mstring _Where, __mstring _What>
    struct _NO_COMMON_DOMAIN_ { };

    template <class _Set>
    using __no_common_domain_t = //
      _NO_COMMON_DOMAIN_<
        __in_which_let_msg<_Set>,
        "The senders returned by Function do not all share a common domain"_mstr>;

    template <class _Set>
    using __try_common_domain_fn = //
      __mtry_catch_q<
        __domain::__common_domain_t,
        __mcompose<__mbind_front_q<__mexception, __no_common_domain_t<_Set>>, __q<_WITH_SENDERS_>>>;

    // Compute all the domains of all the result senders and make sure they're all the same
    template <class _Set, class _Child, class _Fun, class _Env, class _Sched>
    using __result_domain_t = //
      __gather_completions_for<
        _Set,
        _Child,
        _Env,
        __result_sender_fn<_Fun, _Set, _Env, _Sched>,
        __try_common_domain_fn<_Set>>;

    template <class _LetTag, class _Env>
    auto __mk_transform_env_fn(const _Env& __env) noexcept {
      using _Set = __t<_LetTag>;
      return [&]<class _Fun, class _Child>(__ignore, _Fun&&, _Child&& __child) -> decltype(auto) {
        using __completions_t = __completion_signatures_of_t<_Child, _Env>;
        if constexpr (__merror<__completions_t>) {
          return __completions_t();
        } else {
          using _Scheduler = __completion_sched<_Child, _Set>;
          if constexpr (__unknown_context<_Scheduler>) {
            return (__env);
          } else {
            return __env::__join(
              __env::__with(
                get_completion_scheduler<_Set>(stdexec::get_env(__child)), get_scheduler),
              __env::__without(__env, get_domain));
          }
        }
      };
    }

    template <class _LetTag, class _Env>
    auto __mk_transform_sender_fn(const _Env&) noexcept {
      using _Set = __t<_LetTag>;
      return []<class _Fun, class _Child>(__ignore, _Fun&& __fun, _Child&& __child) {
        using __completions_t = __completion_signatures_of_t<_Child, _Env>;
        if constexpr (__merror<__completions_t>) {
          return __completions_t();
        } else {
          using _Sched = __completion_sched<_Child, _Set>;
          using _Domain = __result_domain_t<_Set, _Child, _Fun, _Env, _Sched>;
          if constexpr (__merror<_Domain>) {
            return _Domain();
          } else if constexpr (same_as<_Domain, dependent_domain>) {
            using _Domain2 = __late_domain_of_t<_Child, _Env>;
            return __make_sexpr<__let_t<_Set, _Domain2>>(
              static_cast<_Fun&&>(__fun), static_cast<_Child&&>(__child));
          } else {
            static_assert(!same_as<_Domain, __none_such>);
            return __make_sexpr<__let_t<_Set, _Domain>>(
              static_cast<_Fun&&>(__fun), static_cast<_Child&&>(__child));
          }
        }
      };
    }

    template <class _Receiver, class _Fun, class _Set, class _Sched, class... _Tuples>
    struct __let_state {
      using __fun_t = _Fun;
      using __sched_t = _Sched;

      using __result_variant = std::variant<std::monostate, _Tuples...>;

      using __op_state_variant = //
        __minvoke<
          __transform<__uncurry<__op_state_for<_Receiver, _Fun, _Set, _Sched>>, __nullable_variant_t>,
          _Tuples...>;

      template <class _ResultSender, class _OpState>
      auto __get_result_receiver(const _ResultSender&, _OpState& __op_state) -> decltype(auto) {
        if constexpr (__need_receiver_ref<_ResultSender, _Receiver, _Sched>) {
          return __receiver_ref_t<_ResultSender, _Receiver, _Sched>{__op_state};
        } else {
          _Receiver& __rcvr = __op_state.__rcvr_;
          if constexpr (__unknown_context<_Sched>) {
            return static_cast<_Receiver&&>(__rcvr);
          } else {
            return __receiver_with_sched{static_cast<_Receiver&&>(__rcvr), this->__sched_};
          }
        }
      }

      STDEXEC_IMMOVABLE_NO_UNIQUE_ADDRESS _Fun __fun_;
      STDEXEC_IMMOVABLE_NO_UNIQUE_ADDRESS _Sched __sched_;
      __result_variant __args_;
      __op_state_variant __op_state3_;
    };

    template <class _Set, class _Domain>
    struct __let_t {
      using __domain_t = _Domain;
      using __t = _Set;

      template <sender _Sender, __movable_value _Fun>
      auto operator()(_Sender&& __sndr, _Fun __fun) const -> __well_formed_sender auto {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<__let_t<_Set>>(static_cast<_Fun&&>(__fun), static_cast<_Sender&&>(__sndr)));
      }

      template <class _Fun>
      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()(_Fun __fun) const -> __binder_back<__let_t, _Fun> {
        return {{static_cast<_Fun&&>(__fun)}};
      }

      using _Sender = __1;
      using _Function = __0;
      using __legacy_customizations_t = __types<
        tag_invoke_t(
          __let_t,
          get_completion_scheduler_t<set_value_t>(get_env_t(const _Sender&)),
          _Sender,
          _Function),
        tag_invoke_t(__let_t, _Sender, _Function)>;

      template <sender_expr_for<__let_t<_Set>> _Sender, class _Env>
      static auto transform_env(_Sender&& __sndr, const _Env& __env) -> decltype(auto) {
        return __sexpr_apply(
          static_cast<_Sender&&>(__sndr), __mk_transform_env_fn<__let_t<_Set>>(__env));
      }

      template <sender_expr_for<__let_t<_Set>> _Sender, class _Env>
        requires same_as<__early_domain_of_t<_Sender>, dependent_domain>
      static auto transform_sender(_Sender&& __sndr, const _Env& __env) -> decltype(auto) {
        return __sexpr_apply(
          static_cast<_Sender&&>(__sndr), __mk_transform_sender_fn<__let_t<_Set>>(__env));
      }
    };

    template <class _Set, class _Domain>
    struct __let_impl : __sexpr_defaults {
      static constexpr auto get_attrs = //
        []<class _Child>(__ignore, const _Child& __child) noexcept {
          return __env::__join(__env::__with(_Domain(), get_domain), stdexec::get_env(__child));
        };

      static constexpr auto get_completion_signatures = //
        []<class _Self, class _Env>(_Self&&, _Env&&) noexcept
        -> __completions<__child_of<_Self>, _Env, __let_t<_Set, _Domain>, __data_of<_Self>> {
        static_assert(sender_expr_for<_Self, __let_t<_Set, _Domain>>);
        return {};
      };

      static constexpr auto get_state = //
        []<class _Sender, class _Receiver>(_Sender&& __sndr, _Receiver&) {
          static_assert(sender_expr_for<_Sender, __let_t<_Set, _Domain>>);
          using _Fun = __data_of<_Sender>;
          using _Child = __child_of<_Sender>;
          using _Sched = __completion_sched<_Child, _Set>;
          using __mk_let_state = __mbind_front_q<__let_state, _Receiver, _Fun, _Set, _Sched>;

          using __let_state_t = __gather_completions_for<
            _Set,
            _Child,
            env_of_t<_Receiver>,
            __q<__decayed_tuple>,
            __mk_let_state>;

          _Sched __sched =
            query_or(get_completion_scheduler<_Set>, stdexec::get_env(__sndr), __none_such());
          return __let_state_t{
            __sndr.apply(static_cast<_Sender&&>(__sndr), __detail::__get_data()), __sched};
        };

      template <class _State, class _OpState, class... _As>
      static void __bind_(_State& __state, _OpState& __op_state, _As&&... __as) {
        auto& __args =
          __state.__args_.template emplace<__decayed_tuple<_As...>>(static_cast<_As&&>(__as)...);
        auto __sndr2 = __apply(std::move(__state.__fun_), __args);
        auto __rcvr2 = __state.__get_result_receiver(__sndr2, __op_state);
        auto __mkop = [&] {
          return stdexec::connect(std::move(__sndr2), std::move(__rcvr2));
        };
        auto& __op2 = __state.__op_state3_.template emplace<decltype(__mkop())>(__conv{__mkop});
        stdexec::start(__op2);
      }

      template <class _OpState, class... _As>
      static void __bind(_OpState& __op_state, _As&&... __as) noexcept {
        auto& __state = __op_state.__state_;
        auto& __rcvr = __op_state.__rcvr_;
        using _State = __decay_t<decltype(__state)>;
        using _Receiver = __decay_t<decltype(__rcvr)>;
        using _Fun = typename _State::__fun_t;
        using _Sched = typename _State::__sched_t;
        using _ResultSender =
          __minvoke<__result_sender_fn<_Fun, _Set, env_of_t<_Receiver>, _Sched>, _As...>;
        if constexpr (
          (__nothrow_decay_copyable<_As> && ...) && __nothrow_callable<_Fun, _As...>
          && __nothrow_connectable_receiver_ref<_ResultSender, env_of_t<_Receiver>, _Sched>) {
          __bind_(__state, __op_state, static_cast<_As&&>(__as)...);
        } else {
          try {
            __bind_(__state, __op_state, static_cast<_As&&>(__as)...);
          } catch (...) {
            set_error(std::move(__rcvr), std::current_exception());
          }
        }
      }

      static constexpr auto complete = //
        []<class _OpState, class _Tag, class... _As>(
          __ignore,
          _OpState& __op_state,
          _Tag,
          _As&&... __as) noexcept -> void {
        if constexpr (std::same_as<_Tag, _Set>) {
          __bind(__op_state, static_cast<_As&&>(__as)...);
        } else {
          _Tag()(std::move(__op_state.__rcvr_), static_cast<_As&&>(__as)...);
        }
      };
    };
  } // namespace __let

  using let_value_t = __let::__let_t<set_value_t>;
  inline constexpr let_value_t let_value{};

  using let_error_t = __let::__let_t<set_error_t>;
  inline constexpr let_error_t let_error{};

  using let_stopped_t = __let::__let_t<set_stopped_t>;
  inline constexpr let_stopped_t let_stopped{};

  template <class _Set, class _Domain>
  struct __sexpr_impl<__let::__let_t<_Set, _Domain>> : __let::__let_impl<_Set, _Domain> { };

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.stopped_as_optional]
  // [execution.senders.adaptors.stopped_as_error]
  namespace __stopped_as_xxx {
    struct stopped_as_optional_t {
      template <sender _Sender>
      auto operator()(_Sender&& __sndr) const {
        return __make_sexpr<stopped_as_optional_t>(__(), static_cast<_Sender&&>(__sndr));
      }

      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()() const noexcept -> __binder_back<stopped_as_optional_t> {
        return {};
      }
    };

    struct __stopped_as_optional_impl : __sexpr_defaults {
      template <class... _Tys>
        requires(sizeof...(_Tys) == 1)
      using __set_value_t = completion_signatures<set_value_t(std::optional<__decay_t<_Tys>>...)>;

      template <class _Ty>
      using __set_error_t = completion_signatures<set_error_t(_Ty)>;

      static constexpr auto get_completion_signatures =       //
        []<class _Self, class _Env>(_Self&&, _Env&&) noexcept //
        -> make_completion_signatures<
          __child_of<_Self>,
          _Env,
          completion_signatures<set_error_t(std::exception_ptr)>,
          __set_value_t,
          __set_error_t,
          completion_signatures<>> {
        static_assert(sender_expr_for<_Self, stopped_as_optional_t>);
        return {};
      };

      static constexpr auto get_state = //
        []<class _Self, class _Receiver>(_Self&&, _Receiver&) noexcept
        requires __single_typed_sender<__child_of<_Self>, env_of_t<_Receiver>>
      {
        static_assert(sender_expr_for<_Self, stopped_as_optional_t>);
        using _Value = __decay_t<__single_sender_value_t<__child_of<_Self>, env_of_t<_Receiver>>>;
        return __mtype<_Value>();
      };

      static constexpr auto complete = //
        []<class _State, class _Receiver, __completion_tag _Tag, class... _Args>(
          __ignore,
          _State&,
          _Receiver& __rcvr,
          _Tag,
          _Args&&... __args) noexcept -> void {
        if constexpr (same_as<_Tag, set_value_t>) {
          try {
            static_assert(constructible_from<__t<_State>, _Args...>);
            stdexec::set_value(
              static_cast<_Receiver&&>(__rcvr),
              std::optional<__t<_State>>{static_cast<_Args&&>(__args)...});
          } catch (...) {
            stdexec::set_error(static_cast<_Receiver&&>(__rcvr), std::current_exception());
          }
        } else if constexpr (same_as<_Tag, set_error_t>) {
          stdexec::set_error(static_cast<_Receiver&&>(__rcvr), static_cast<_Args&&>(__args)...);
        } else {
          stdexec::set_value(
            static_cast<_Receiver&&>(__rcvr), std::optional<__t<_State>>{std::nullopt});
        }
      };
    };

    struct stopped_as_error_t {
      template <sender _Sender, __movable_value _Error>
      auto operator()(_Sender&& __sndr, _Error __err) const {
        return static_cast<_Sender&&>(__sndr)
             | let_stopped([__err2 = static_cast<_Error&&>(__err)]() mutable //
                           noexcept(std::is_nothrow_move_constructible_v<_Error>) {
                             return just_error(static_cast<_Error&&>(__err2));
                           });
      }

      template <__movable_value _Error>
      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()(_Error __err) const -> __binder_back<stopped_as_error_t, _Error> {
        return {{static_cast<_Error&&>(__err)}};
      }
    };
  } // namespace __stopped_as_xxx

  using __stopped_as_xxx::stopped_as_optional_t;
  inline constexpr stopped_as_optional_t stopped_as_optional{};
  using __stopped_as_xxx::stopped_as_error_t;
  inline constexpr stopped_as_error_t stopped_as_error{};

  template <>
  struct __sexpr_impl<stopped_as_optional_t> : __stopped_as_xxx::__stopped_as_optional_impl { };

  /////////////////////////////////////////////////////////////////////////////
  // run_loop
  namespace __loop {
    class run_loop;

    struct __task : __immovable {
      __task* __next_ = this;

      union {
        void (*__execute_)(__task*) noexcept;
        __task* __tail_;
      };

      void __execute() noexcept {
        (*__execute_)(this);
      }
    };

    template <class _ReceiverId>
    struct __operation {
      using _Receiver = stdexec::__t<_ReceiverId>;

      struct __t : __task {
        using __id = __operation;

        run_loop* __loop_;
        STDEXEC_ATTRIBUTE((no_unique_address))
        _Receiver __rcvr_;

        static void __execute_impl(__task* __p) noexcept {
          auto& __rcvr = static_cast<__t*>(__p)->__rcvr_;
          try {
            if (get_stop_token(get_env(__rcvr)).stop_requested()) {
              set_stopped(static_cast<_Receiver&&>(__rcvr));
            } else {
              set_value(static_cast<_Receiver&&>(__rcvr));
            }
          } catch (...) {
            set_error(static_cast<_Receiver&&>(__rcvr), std::current_exception());
          }
        }

        explicit __t(__task* __tail) noexcept
          : __task{.__tail_ = __tail} {
        }

        __t(__task* __next, run_loop* __loop, _Receiver __rcvr)
          : __task{{}, __next, {&__execute_impl}}
          , __loop_{__loop}
          , __rcvr_{static_cast<_Receiver&&>(__rcvr)} {
        }

        STDEXEC_ATTRIBUTE((always_inline))
        STDEXEC_MEMFN_DECL(
          void start)(this __t& __self) noexcept {
          __self.__start_();
        }

        void __start_() noexcept;
      };
    };

    class run_loop {
      template <class... Ts>
      using __completion_signatures_ = completion_signatures<Ts...>;

      template <class>
      friend struct __operation;
     public:
      struct __scheduler {
        using __t = __scheduler;
        using __id = __scheduler;
        auto operator==(const __scheduler&) const noexcept -> bool = default;

       private:
        struct __schedule_task {
          using __t = __schedule_task;
          using __id = __schedule_task;
          using sender_concept = sender_t;
          using completion_signatures = //
            __completion_signatures_<set_value_t(), set_error_t(std::exception_ptr), set_stopped_t()>;

         private:
          friend __scheduler;

          template <class _Receiver>
          using __operation = stdexec::__t<__operation<stdexec::__id<_Receiver>>>;

          template <class _Receiver>
          STDEXEC_MEMFN_DECL(auto connect)(this const __schedule_task& __self, _Receiver __rcvr)
            -> __operation<_Receiver> {
            return __self.__connect_(static_cast<_Receiver&&>(__rcvr));
          }

          template <class _Receiver>
          auto __connect_(_Receiver&& __rcvr) const -> __operation<_Receiver> {
            return {&__loop_->__head_, __loop_, static_cast<_Receiver&&>(__rcvr)};
          }

          struct __env {
            run_loop* __loop_;

            template <class _CPO>
            STDEXEC_MEMFN_DECL(auto query)(this const __env& __self, get_completion_scheduler_t<_CPO>) noexcept
              -> __scheduler {
              return __self.__loop_->get_scheduler();
            }
          };

          STDEXEC_MEMFN_DECL(auto get_env)(this const __schedule_task& __self) noexcept -> __env {
            return __env{__self.__loop_};
          }

          explicit __schedule_task(run_loop* __loop) noexcept
            : __loop_(__loop) {
          }

          run_loop* const __loop_;
        };

        friend run_loop;

        explicit __scheduler(run_loop* __loop) noexcept
          : __loop_(__loop) {
        }

        STDEXEC_MEMFN_DECL(auto schedule)(this const __scheduler& __self) noexcept -> __schedule_task {
          return __self.__schedule();
        }

        STDEXEC_MEMFN_DECL(auto query)(this const __scheduler&, get_forward_progress_guarantee_t) noexcept
          -> stdexec::forward_progress_guarantee {
          return stdexec::forward_progress_guarantee::parallel;
        }

        // BUGBUG NOT TO SPEC
        STDEXEC_MEMFN_DECL(
          auto execute_may_block_caller)(this const __scheduler&) noexcept -> bool {
          return false;
        }

        [[nodiscard]]
        auto __schedule() const noexcept -> __schedule_task {
          return __schedule_task{__loop_};
        }

        run_loop* __loop_;
      };

      auto get_scheduler() noexcept -> __scheduler {
        return __scheduler{this};
      }

      void run();

      void finish();

     private:
      void __push_back_(__task* __task);
      auto __pop_front_() -> __task*;

      std::mutex __mutex_;
      std::condition_variable __cv_;
      __task __head_{.__tail_ = &__head_};
      bool __stop_ = false;
    };

    template <class _ReceiverId>
    inline void __operation<_ReceiverId>::__t::__start_() noexcept {
      try {
        __loop_->__push_back_(this);
      } catch (...) {
        set_error(static_cast<_Receiver&&>(__rcvr_), std::current_exception());
      }
    }

    inline void run_loop::run() {
      for (__task* __task; (__task = __pop_front_()) != &__head_;) {
        __task->__execute();
      }
    }

    inline void run_loop::finish() {
      std::unique_lock __lock{__mutex_};
      __stop_ = true;
      __cv_.notify_all();
    }

    inline void run_loop::__push_back_(__task* __task) {
      std::unique_lock __lock{__mutex_};
      __task->__next_ = &__head_;
      __head_.__tail_ = __head_.__tail_->__next_ = __task;
      __cv_.notify_one();
    }

    inline auto run_loop::__pop_front_() -> __task* {
      std::unique_lock __lock{__mutex_};
      __cv_.wait(__lock, [this] { return __head_.__next_ != &__head_ || __stop_; });
      if (__head_.__tail_ == __head_.__next_)
        __head_.__tail_ = &__head_;
      return std::exchange(__head_.__next_, __head_.__next_->__next_);
    }
  } // namespace __loop

  // NOT TO SPEC
  using run_loop = __loop::run_loop;

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.schedule_from]
  namespace __schedule_from {
    template <class... _Ts>
    using __value_tuple = __tup::__tuple_for<__decay_t<_Ts>...>;

    // Compute a variant type that is capable of storing the results of the
    // input sender when it completes. The variant has type:
    //   variant<
    //     monostate,
    //     tuple<set_stopped_t>,
    //     tuple<set_value_t, __decay_t<_Values1>...>,
    //     tuple<set_value_t, __decay_t<_Values2>...>,
    //        ...
    //     tuple<set_error_t, __decay_t<_Error1>>,
    //     tuple<set_error_t, __decay_t<_Error2>>,
    //        ...
    //   >
    template <class _CvrefSender, class _Env>
    using __variant_for_t = __compl_sigs::__maybe_for_all_sigs<
      __completion_signatures_of_t<_CvrefSender, _Env>,
      __q<__value_tuple>,
      __nullable_variant_t>;

    template <class _Tag>
    using __decay_signature =
      __transform<__q<__decay_t>, __mcompose<__q<completion_signatures>, __qf<_Tag>>>;

    template <class... _Ts>
    using __all_nothrow_decay_copyable_ = __mbool<(__nothrow_decay_copyable<_Ts> && ...)>;

    template <class _CvrefSender, class _Env>
    using __all_values_and_errors_nothrow_decay_copyable = //
      __compl_sigs::__maybe_for_all_sigs<
        __completion_signatures_of_t<_CvrefSender, _Env>,
        __q<__all_nothrow_decay_copyable_>,
        __q<__mand>>;

    template <class _CvrefSender, class _Env>
    using __with_error_t = //
      __if<
        __all_values_and_errors_nothrow_decay_copyable<_CvrefSender, _Env>,
        completion_signatures<>,
        __with_exception_ptr>;

    template <class _Scheduler, class _CvrefSender, class _Env>
    using __completions_t = //
      __try_make_completion_signatures<
        _CvrefSender,
        _Env,
        __try_make_completion_signatures<
          schedule_result_t<_Scheduler>,
          _Env,
          __with_error_t<_CvrefSender, _Env>,
          __mconst<completion_signatures<>>>,
        __decay_signature<set_value_t>,
        __decay_signature<set_error_t>>;

    template <class _SchedulerId>
    struct __environ {
      using _Scheduler = stdexec::__t<_SchedulerId>;

      struct __t
        : __env::__with<
            stdexec::__t<_SchedulerId>,
            get_completion_scheduler_t<set_value_t>,
            get_completion_scheduler_t<set_stopped_t>> {
        using __id = __environ;

        explicit __t(_Scheduler __sched) noexcept
          : __t::__with{std::move(__sched)} {
        }

        template <same_as<get_domain_t> _Key>
        friend auto tag_invoke(_Key, const __t& __self) noexcept {
          return query_or(get_domain, __self.__value_, default_domain());
        }
      };
    };

    template <class _Scheduler, class _Sexpr, class _Receiver>
    struct __state;

    // This receiver is to be completed on the execution context
    // associated with the scheduler. When the source sender
    // completes, the completion information is saved off in the
    // operation state so that when this receiver completes, it can
    // read the completion out of the operation state and forward it
    // to the output receiver after transitioning to the scheduler's
    // context.
    template <class _Scheduler, class _Sexpr, class _Receiver>
    struct __receiver2 : receiver_adaptor<__receiver2<_Scheduler, _Sexpr, _Receiver>> {
      explicit __receiver2(__state<_Scheduler, _Sexpr, _Receiver>* __state) noexcept
        : __state_{__state} {
      }

      auto base() && noexcept -> _Receiver&& {
        return std::move(__state_->__receiver());
      }

      auto base() const & noexcept -> const _Receiver& {
        return __state_->__receiver();
      }

      void set_value() && noexcept {
        STDEXEC_ASSERT(!__state_->__data_.valueless_by_exception());
        std::visit(
          [__state = __state_]<class _Tup>(_Tup& __tupl) noexcept -> void {
            if constexpr (same_as<_Tup, std::monostate>) {
              std::terminate(); // reaching this indicates a bug in schedule_from
            } else {
              __tup::__apply(
                [&]<class... _Args>(auto __tag, _Args&... __args) noexcept -> void {
                  __tag(std::move(__state->__receiver()), static_cast<_Args&&>(__args)...);
                },
                __tupl);
            }
          },
          __state_->__data_);
      }

      __state<_Scheduler, _Sexpr, _Receiver>* __state_;
    };

    template <class _Scheduler, class _Sexpr, class _Receiver>
    struct __state
      : __enable_receiver_from_this<_Sexpr, _Receiver>
      , __immovable {
      using __variant_t = __variant_for_t<__child_of<_Sexpr>, env_of_t<_Receiver>>;
      using __receiver2_t = __receiver2<_Scheduler, _Sexpr, _Receiver>;

      __variant_t __data_;
      connect_result_t<schedule_result_t<_Scheduler>, __receiver2_t> __state2_;
      STDEXEC_APPLE_CLANG(__state* __self_;)

      explicit __state(_Scheduler __sched)
        : __data_()
        , __state2_(connect(schedule(__sched), __receiver2_t{this}))
            STDEXEC_APPLE_CLANG(, __self_(this)) {
      }
    };

    struct schedule_from_t {
      template <scheduler _Scheduler, sender _Sender>
      auto operator()(_Scheduler&& __sched, _Sender&& __sndr) const -> __well_formed_sender auto {
        using _Env = __t<__environ<__id<__decay_t<_Scheduler>>>>;
        auto __env = _Env{{static_cast<_Scheduler&&>(__sched)}};
        auto __domain = query_or(get_domain, __sched, default_domain());
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<schedule_from_t>(std::move(__env), static_cast<_Sender&&>(__sndr)));
      }

      using _Sender = __1;
      using _Env = __0;
      using __legacy_customizations_t = __types<
        tag_invoke_t(schedule_from_t, get_completion_scheduler_t<set_value_t>(_Env&), _Sender)>;
    };

    struct __schedule_from_impl : __sexpr_defaults {
      template <class _Sender>
      using __scheduler_t =
        __decay_t<__call_result_t<get_completion_scheduler_t<set_value_t>, env_of_t<_Sender>>>;

      static constexpr auto get_attrs = //
        []<class _Data, class _Child>(const _Data& __data, const _Child& __child) noexcept {
          return __env::__join(__data, stdexec::get_env(__child));
        };

      static constexpr auto get_completion_signatures = //
        []<class _Sender, class _Env>(_Sender&&, const _Env&) noexcept
        -> __completions_t<__scheduler_t<_Sender>, __child_of<_Sender>, _Env> {
        static_assert(sender_expr_for<_Sender, schedule_from_t>);
        return {};
      };

      static constexpr auto get_state =
        []<class _Sender, class _Receiver>(_Sender&& __sndr, _Receiver&) {
          static_assert(sender_expr_for<_Sender, schedule_from_t>);
          auto __sched = get_completion_scheduler<set_value_t>(stdexec::get_env(__sndr));
          using _Scheduler = decltype(__sched);
          return __state<_Scheduler, _Sender, _Receiver>{__sched};
        };

      static constexpr auto complete = []<class _Tag, class... _Args>(
                                         __ignore,
                                         auto& __state,
                                         auto& __rcvr,
                                         _Tag,
                                         _Args&&... __args) noexcept -> void {
        STDEXEC_APPLE_CLANG(__state.__self_ == &__state ? void() : std::terminate());
        // Write the tag and the args into the operation state so that
        // we can forward the completion from within the scheduler's
        // execution context.
        using __async_result = __value_tuple<_Tag, _Args...>;
        constexpr bool __nothrow_ =
          noexcept(__async_result{_Tag(), static_cast<_Args&&>(__args)...});
        auto __emplace_result = [&]() noexcept(__nothrow_) {
          return __async_result{_Tag(), static_cast<_Args&&>(__args)...};
        };
        if constexpr (__nothrow_) {
          __state.__data_.template emplace<__async_result>(__conv{__emplace_result});
        } else {
          try {
            __state.__data_.template emplace<__async_result>(__conv{__emplace_result});
          } catch (...) {
            set_error(std::move(__rcvr), std::current_exception());
            return;
          }
        }
        // Enqueue the schedule operation so the completion happens
        // on the scheduler's execution context.
        stdexec::start(__state.__state2_);
      };
    };
  } // namespace __schedule_from

  using __schedule_from::schedule_from_t;
  inline constexpr schedule_from_t schedule_from{};

  template <>
  struct __sexpr_impl<schedule_from_t> : __schedule_from::__schedule_from_impl { };

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.continue_on]
  namespace __continue_on {
    using __schedule_from::__environ;

    template <class _Env>
    using __scheduler_t = __result_of<get_completion_scheduler<set_value_t>, _Env>;

    template <class _Sender>
    using __lowered_t = //
      __result_of<schedule_from, __scheduler_t<__data_of<_Sender>>, __child_of<_Sender>>;

    struct continue_on_t {
      template <sender _Sender, scheduler _Scheduler>
      auto operator()(_Sender&& __sndr, _Scheduler&& __sched) const -> __well_formed_sender auto {
        auto __domain = __get_early_domain(__sndr);
        using _Env = __t<__environ<__id<__decay_t<_Scheduler>>>>;
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<continue_on_t>(
            _Env{{static_cast<_Scheduler&&>(__sched)}}, static_cast<_Sender&&>(__sndr)));
      }

      template <scheduler _Scheduler>
      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()(_Scheduler&& __sched) const
        -> __binder_back<continue_on_t, __decay_t<_Scheduler>> {
        return {{static_cast<_Scheduler&&>(__sched)}};
      }

      //////////////////////////////////////////////////////////////////////////////////////////////
      using _Env = __0;
      using _Sender = __1;
      using __legacy_customizations_t = //
        __types<
          tag_invoke_t(
            continue_on_t,
            get_completion_scheduler_t<set_value_t>(get_env_t(const _Sender&)),
            _Sender,
            get_completion_scheduler_t<set_value_t>(_Env)),
          tag_invoke_t(continue_on_t, _Sender, get_completion_scheduler_t<set_value_t>(_Env))>;

      template <class _Env>
      static auto __transform_sender_fn(const _Env&) {
        return [&]<class _Data, class _Child>(__ignore, _Data&& __data, _Child&& __child) {
          auto __sched = get_completion_scheduler<set_value_t>(__data);
          return schedule_from(std::move(__sched), static_cast<_Child&&>(__child));
        };
      }

      template <class _Sender, class _Env>
      static auto transform_sender(_Sender&& __sndr, const _Env& __env) {
        return __sexpr_apply(static_cast<_Sender&&>(__sndr), __transform_sender_fn(__env));
      }
    };

    struct __continue_on_impl : __sexpr_defaults {
      static constexpr auto get_attrs = //
        []<class _Data, class _Child>(const _Data& __data, const _Child& __child) noexcept
        -> decltype(auto) {
        return __env::__join(__data, stdexec::get_env(__child));
      };
    };
  } // namespace __continue_on

  using __continue_on::continue_on_t;
  inline constexpr continue_on_t continue_on{};

  using transfer_t = continue_on_t;
  inline constexpr transfer_t transfer{};

  template <>
  struct __sexpr_impl<continue_on_t> : __continue_on::__continue_on_impl { };

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.transfer_just]
  namespace __transfer_just {
    // This is a helper for finding legacy cusutomizations of transfer_just.
    inline auto __transfer_just_tag_invoke() {
      return []<class... _Ts>(_Ts&&... __ts) -> tag_invoke_result_t<transfer_just_t, _Ts...> {
        return tag_invoke(transfer_just, static_cast<_Ts&&>(__ts)...);
      };
    }

    template <class _Env>
    auto __make_transform_fn(const _Env&) {
      return [&]<class _Scheduler, class... _Values>(_Scheduler&& __sched, _Values&&... __vals) {
        return transfer(just(static_cast<_Values&&>(__vals)...), static_cast<_Scheduler&&>(__sched));
      };
    }

    template <class _Env>
    auto __transform_sender_fn(const _Env& __env) {
      return [&]<class _Data>(__ignore, _Data&& __data) {
        return __tup::__apply(__make_transform_fn(__env), static_cast<_Data&&>(__data));
      };
    }

    struct transfer_just_t {
      using _Data = __0;
      using __legacy_customizations_t = //
        __types<__tup::__apply_t(decltype(__transfer_just_tag_invoke()), _Data)>;

      template <scheduler _Scheduler, __movable_value... _Values>
      auto
        operator()(_Scheduler&& __sched, _Values&&... __vals) const -> __well_formed_sender auto {
        auto __domain = query_or(get_domain, __sched, default_domain());
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<transfer_just_t>(
            __tuple{static_cast<_Scheduler&&>(__sched), static_cast<_Values&&>(__vals)...}));
      }

      template <class _Sender, class _Env>
      static auto transform_sender(_Sender&& __sndr, const _Env& __env) {
        return __sexpr_apply(static_cast<_Sender&&>(__sndr), __transform_sender_fn(__env));
      }
    };

    inline auto __make_env_fn() noexcept {
      return []<class _Scheduler>(const _Scheduler& __sched, const auto&...) noexcept {
        using _Env = __t<__schedule_from::__environ<__id<_Scheduler>>>;
        return _Env{__sched};
      };
    }

    struct __transfer_just_impl : __sexpr_defaults {
      static constexpr auto get_attrs = //
        []<class _Data>(const _Data& __data) noexcept {
          return __tup::__apply(__make_env_fn(), __data);
        };
    };
  } // namespace __transfer_just

  using __transfer_just::transfer_just_t;
  inline constexpr transfer_just_t transfer_just{};

  template <>
  struct __sexpr_impl<transfer_just_t> : __transfer_just::__transfer_just_impl { };

  //////////////////////////////////////////////////////////////////////////////////////////////////
  // __write adaptor
  namespace __write_ {
    struct __write_t {
      template <sender _Sender, class... _Envs>
      auto operator()(_Sender&& __sndr, _Envs... __envs) const {
        return __make_sexpr<__write_t>(
          __env::__join(static_cast<_Envs&&>(__envs)...), static_cast<_Sender&&>(__sndr));
      }

      template <class... _Envs>
      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()(_Envs... __envs) const -> __binder_back<__write_t, _Envs...> {
        return {{static_cast<_Envs&&>(__envs)...}};
      }

      template <class _Env>
      STDEXEC_ATTRIBUTE((always_inline))
      static auto
        __transform_env_fn(_Env&& __env) noexcept {
        return [&](__ignore, const auto& __state, __ignore) noexcept {
          return __env::__join(__state, static_cast<_Env&&>(__env));
        };
      }

      template <sender_expr_for<__write_t> _Self, class _Env>
      static auto transform_env(const _Self& __self, _Env&& __env) noexcept {
        return __sexpr_apply(__self, __transform_env_fn(static_cast<_Env&&>(__env)));
      }
    };

    struct __write_impl : __sexpr_defaults {
      static constexpr auto get_env = //
        [](__ignore, const auto& __state, const auto& __rcvr) noexcept {
          return __env::__join(__state, stdexec::get_env(__rcvr));
        };

      static constexpr auto get_completion_signatures = //
        []<class _Self, class _Env>(_Self&&, _Env&&) noexcept
        -> stdexec::__completion_signatures_of_t<
          __child_of<_Self>,
          __env::__join_t<const __decay_t<__data_of<_Self>>&, _Env>> {
        static_assert(sender_expr_for<_Self, __write_t>);
        return {};
      };
    };
  } // namespace __write_

  using __write_::__write_t;
  inline constexpr __write_t __write{};

  template <>
  struct __sexpr_impl<__write_t> : __write_::__write_impl { };

  namespace __detail {
    template <class _Env, class _Scheduler>
    STDEXEC_ATTRIBUTE((always_inline))
    auto
      __mkenv_sched(_Env&& __env, _Scheduler __sched) {
      auto __env2 = __env::__join(
        __env::__with(__sched, get_scheduler),
        __env::__without(static_cast<_Env&&>(__env), get_domain));
      using _Env2 = decltype(__env2);

      struct __env_t : _Env2 { };

      return __env_t{static_cast<_Env2&&>(__env2)};
    }

    template <class _Ty, class = __name_of<__decay_t<_Ty>>>
    struct __always {
      _Ty __val_;

      auto operator()() noexcept -> _Ty {
        return static_cast<_Ty&&>(__val_);
      }
    };

    template <class _Ty>
    __always(_Ty) -> __always<_Ty>;
  } // namespace __detail

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.start_on]
  namespace __start_on {
    struct start_on_t {
      using _Sender = __1;
      using _Scheduler = __0;
      using __legacy_customizations_t = __types<tag_invoke_t(start_on_t, _Scheduler, _Sender)>;

      template <scheduler _Scheduler, sender _Sender>
      auto operator()(_Scheduler&& __sched, _Sender&& __sndr) const -> __well_formed_sender auto {
        auto __domain = query_or(get_domain, __sched, default_domain());
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<start_on_t>(
            static_cast<_Scheduler&&>(__sched), static_cast<_Sender&&>(__sndr)));
      }

      template <class _Env>
      STDEXEC_ATTRIBUTE((always_inline))
      static auto
        __transform_env_fn(_Env&& __env) noexcept {
        return [&](__ignore, auto __sched, __ignore) noexcept {
          return __detail::__mkenv_sched(static_cast<_Env&&>(__env), __sched);
        };
      }

      template <class _Sender, class _Env>
      static auto transform_env(const _Sender& __sndr, _Env&& __env) noexcept {
        return __sexpr_apply(__sndr, __transform_env_fn(static_cast<_Env&&>(__env)));
      }

      template <class _Sender, class _Env>
      static auto transform_sender(_Sender&& __sndr, const _Env&) {
        return __sexpr_apply(
          static_cast<_Sender&&>(__sndr),
          []<class _Data, class _Child>(__ignore, _Data&& __data, _Child&& __child) {
            return let_value(schedule(__data), __detail::__always{static_cast<_Child&&>(__child)});
          });
      }
    };
  } // namespace __start_on

  using __start_on::start_on_t;
  inline constexpr start_on_t start_on{};

  using on_t = start_on_t;
  inline constexpr on_t on{};

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.into_variant]
  namespace __into_variant {
    template <class _Sender, class _Env>
      requires sender_in<_Sender, _Env>
    using __into_variant_result_t = value_types_of_t<_Sender, _Env>;

    template <class _Sender, class _Env>
    using __variant_t = __try_value_types_of_t<_Sender, _Env>;

    template <class _Variant>
    using __variant_completions =
      completion_signatures<set_value_t(_Variant), set_error_t(std::exception_ptr)>;

    template <class _Sender, class _Env>
    using __compl_sigs = //
      __try_make_completion_signatures<
        _Sender,
        _Env,
        __meval<__variant_completions, __variant_t<_Sender, _Env>>,
        __mconst<completion_signatures<>>>;

    struct into_variant_t {
      template <sender _Sender>
      auto operator()(_Sender&& __sndr) const -> __well_formed_sender auto {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::transform_sender(
          __domain, __make_sexpr<into_variant_t>(__(), std::forward<_Sender>(__sndr)));
      }

      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()() const noexcept -> __binder_back<into_variant_t> {
        return {};
      }
    };

    struct __into_variant_impl : __sexpr_defaults {
      static constexpr auto get_state = //
        []<class _Self, class _Receiver>(_Self&&, _Receiver&) noexcept {
          using __variant_t = value_types_of_t<__child_of<_Self>, env_of_t<_Receiver>>;
          return __mtype<__variant_t>();
        };

      static constexpr auto complete = //
        []<class _State, class _Receiver, class _Tag, class... _Args>(
          __ignore,
          _State,
          _Receiver& __rcvr,
          _Tag,
          _Args&&... __args) noexcept -> void {
        if constexpr (same_as<_Tag, set_value_t>) {
          using __variant_t = __t<_State>;
          try {
            set_value(
              static_cast<_Receiver&&>(__rcvr),
              __variant_t{std::tuple<_Args&&...>{static_cast<_Args&&>(__args)...}});
          } catch (...) {
            set_error(static_cast<_Receiver&&>(__rcvr), std::current_exception());
          }
        } else {
          _Tag()(static_cast<_Receiver&&>(__rcvr), static_cast<_Args&&>(__args)...);
        }
      };

      static constexpr auto get_completion_signatures =       //
        []<class _Self, class _Env>(_Self&&, _Env&&) noexcept //
        -> __compl_sigs<__child_of<_Self>, _Env> {
        static_assert(sender_expr_for<_Self, into_variant_t>);
        return {};
      };
    };
  } // namespace __into_variant

  using __into_variant::into_variant_t;
  inline constexpr into_variant_t into_variant{};

  template <>
  struct __sexpr_impl<into_variant_t> : __into_variant::__into_variant_impl { };

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.when_all]
  // [execution.senders.adaptors.when_all_with_variant]
  namespace __when_all {
    enum __state_t {
      __started,
      __error,
      __stopped
    };

    struct __on_stop_request {
      inplace_stop_source& __stop_source_;

      void operator()() noexcept {
        __stop_source_.request_stop();
      }
    };

    template <class _Env>
    auto __mkenv(_Env&& __env, const inplace_stop_source& __stop_source) noexcept {
      return __env::__join(
        __env::__with(__stop_source.get_token(), get_stop_token), static_cast<_Env&&>(__env));
    }

    template <class _Env>
    using __env_t = //
      decltype(__mkenv(__declval<_Env>(), __declval<inplace_stop_source&>()));

    template <class _Sender, class _Env>
    concept __max1_sender =
      sender_in<_Sender, _Env>
      && __mvalid<__value_types_of_t, _Sender, _Env, __mconst<int>, __msingle_or<void>>;

    template <
      __mstring _Context = "In stdexec::when_all()..."_mstr,
      __mstring _Diagnostic =
        "The given sender can complete successfully in more that one way. "
        "Use stdexec::when_all_with_variant() instead."_mstr>
    struct _INVALID_WHEN_ALL_ARGUMENT_;

    template <class _Sender, class _Env>
    using __too_many_value_completions_error =
      __mexception<_INVALID_WHEN_ALL_ARGUMENT_<>, _WITH_SENDER_<_Sender>, _WITH_ENVIRONMENT_<_Env>>;

    template <class _Sender, class _Env, class _ValueTuple, class... _Rest>
    using __value_tuple_t = __minvoke<
      __if_c<(0 == sizeof...(_Rest)), __mconst<_ValueTuple>, __q<__too_many_value_completions_error>>,
      _Sender,
      _Env>;

    template <class _Env, class _Sender>
    using __single_values_of_t = //
      __try_value_types_of_t<
        _Sender,
        _Env,
        __transform<__q<__decay_t>, __q<__types>>,
        __mbind_front_q<__value_tuple_t, _Sender, _Env>>;

    template <class _Env, class... _Senders>
    using __set_values_sig_t = //
      __meval<
        completion_signatures,
        __minvoke<__mconcat<__qf<set_value_t>>, __single_values_of_t<_Env, _Senders>...>>;

    template <class... _Args>
    using __all_nothrow_decay_copyable_ = __mbool<(__nothrow_decay_copyable<_Args> && ...)>;

    template <class _Env, class... _Senders>
    using __all_nothrow_decay_copyable = //
      __mand<__compl_sigs::__maybe_for_all_sigs<
        __completion_signatures_of_t<_Senders, _Env>,
        __q<__all_nothrow_decay_copyable_>,
        __q<__mand>>...>;

    template <class _Env, class... _Senders>
    using __completions_t = //
      __concat_completion_signatures_t<
        __if<
          __all_nothrow_decay_copyable<_Env, _Senders...>,
          completion_signatures<set_stopped_t()>,
          completion_signatures<set_stopped_t(), set_error_t(std::exception_ptr)>>,
        __minvoke<
          __with_default<__mbind_front_q<__set_values_sig_t, _Env>, completion_signatures<>>,
          _Senders...>,
        __try_make_completion_signatures<
          _Senders,
          _Env,
          completion_signatures<>,
          __mconst<completion_signatures<>>,
          __mcompose<__q<completion_signatures>, __qf<set_error_t>, __q<__decay_t>>>...>;

    struct __not_an_error { };

    struct __tie_fn {
      template <class... _Ty>
      auto operator()(_Ty&... __vals) noexcept -> std::tuple<_Ty&...> {
        return std::tuple<_Ty&...>{__vals...};
      }
    };

    template <class _Tag, class _Receiver>
    auto __complete_fn(_Tag, _Receiver& __rcvr) noexcept {
      return [&]<class... _Ts>(_Ts&... __ts) noexcept {
        if constexpr (!same_as<__types<_Ts...>, __types<__not_an_error>>) {
          _Tag()(static_cast<_Receiver&&>(__rcvr), static_cast<_Ts&&>(__ts)...);
        }
      };
    }

    template <class _Receiver, class _ValuesTuple>
    void __set_values(_Receiver& __rcvr, _ValuesTuple& __values) noexcept {
      __tup::__apply(
        [&](auto&... __opt_vals) noexcept -> void {
          __apply(
            __complete_fn(set_value, __rcvr), //
            std::tuple_cat(__tup::__apply(__tie_fn{}, *__opt_vals)...));
        },
        __values);
    }

    template <class... Ts>
    using __decayed_custom_tuple = __tup::__tuple_for<__decay_t<Ts>...>;

    template <class _Env, class _Sender>
    using __values_opt_tuple_t = //
      value_types_of_t<_Sender, __env_t<_Env>, __decayed_custom_tuple, std::optional>;

    template <class _Env, __max1_sender<__env_t<_Env>>... _Senders>
    struct __traits {
      // tuple<optional<tuple<Vs1...>>, optional<tuple<Vs2...>>, ...>
      using __values_tuple = //
        __minvoke<
          __with_default<
            __transform<__mbind_front_q<__values_opt_tuple_t, _Env>, __q<__tup::__tuple_for>>,
            __ignore>,
          _Senders...>;

      using __nullable_variant_t_ = __munique<__mbind_front_q<std::variant, __not_an_error>>;

      using __error_types = //
        __minvoke<
          __mconcat<__transform<__q<__decay_t>, __nullable_variant_t_>>,
          error_types_of_t<_Senders, __env_t<_Env>, __types>...>;

      using __errors_variant = //
        __if<
          __all_nothrow_decay_copyable<_Env, _Senders...>,
          __error_types,
          __minvoke<__push_back_unique<__q<std::variant>>, __error_types, std::exception_ptr>>;
    };

    struct _INVALID_ARGUMENTS_TO_WHEN_ALL_ { };

    template <class _ErrorsVariant, class _ValuesTuple, class _StopToken>
    struct __when_all_state {
      using __stop_callback_t = typename _StopToken::template callback_type<__on_stop_request>;

      template <class _Receiver>
      void __arrive(_Receiver& __rcvr) noexcept {
        if (0 == --__count_) {
          __complete(__rcvr);
        }
      }

      template <class _Receiver>
      void __complete(_Receiver& __rcvr) noexcept {
        // Stop callback is no longer needed. Destroy it.
        __on_stop_.reset();
        // All child operations have completed and arrived at the barrier.
        switch (__state_.load(std::memory_order_relaxed)) {
        case __started:
          if constexpr (!same_as<_ValuesTuple, __ignore>) {
            // All child operations completed successfully:
            __when_all::__set_values(__rcvr, __values_);
          }
          break;
        case __error:
          if constexpr (!same_as<_ErrorsVariant, std::variant<std::monostate>>) {
            // One or more child operations completed with an error:
            std::visit(__complete_fn(set_error, __rcvr), __errors_);
          }
          break;
        case __stopped:
          stdexec::set_stopped(static_cast<_Receiver&&>(__rcvr));
          break;
        default:;
        }
      }

      std::atomic<std::size_t> __count_;
      inplace_stop_source __stop_source_{};
      // Could be non-atomic here and atomic_ref everywhere except __completion_fn
      std::atomic<__state_t> __state_{__started};
      _ErrorsVariant __errors_{};
      STDEXEC_ATTRIBUTE((no_unique_address))
      _ValuesTuple __values_{};
      std::optional<__stop_callback_t> __on_stop_{};
    };

    template <class _Env>
    static auto __mk_state_fn(const _Env&) noexcept {
      return [&]<__max1_sender<__env_t<_Env>>... _Child>(__ignore, __ignore, _Child&&...) {
        using _Traits = __traits<_Env, _Child...>;
        using _ErrorsVariant = typename _Traits::__errors_variant;
        using _ValuesTuple = typename _Traits::__values_tuple;
        using _State = __when_all_state<_ErrorsVariant, _ValuesTuple, stop_token_of_t<_Env>>;
        return _State{
          sizeof...(_Child),
          inplace_stop_source{},
          __started,
          _ErrorsVariant{},
          _ValuesTuple{},
          std::nullopt};
      };
    }

    template <class _Env>
    using __mk_state_fn_t = decltype(__when_all::__mk_state_fn(__declval<_Env>()));

    struct when_all_t {
      // Used by the default_domain to find legacy customizations:
      using _Sender = __1;
      using __legacy_customizations_t = //
        __types<tag_invoke_t(when_all_t, _Sender...)>;

      template <sender... _Senders>
        requires __domain::__has_common_domain<_Senders...>
      auto operator()(_Senders&&... __sndrs) const -> __well_formed_sender auto {
        auto __domain = __domain::__common_domain_t<_Senders...>();
        return stdexec::transform_sender(
          __domain, __make_sexpr<when_all_t>(__(), static_cast<_Senders&&>(__sndrs)...));
      }
    };

    struct __when_all_impl : __sexpr_defaults {
      template <class _Self, class _Env>
      using __error_t = __mexception<
        _INVALID_ARGUMENTS_TO_WHEN_ALL_,
        __children_of<_Self, __q<_WITH_SENDERS_>>,
        _WITH_ENVIRONMENT_<_Env>>;

      template <class _Self, class _Env>
      using __completions = //
        __children_of<_Self, __mbind_front_q<__completions_t, __env_t<_Env>>>;

      static constexpr auto get_attrs = //
        []<class... _Child>(__ignore, const _Child&...) noexcept {
          using _Domain = __domain::__common_domain_t<_Child...>;
          if constexpr (same_as<_Domain, default_domain>) {
            return empty_env();
          } else {
            return __env::__with(_Domain(), get_domain);
          }
        };

      static constexpr auto get_completion_signatures = //
        []<class _Self, class _Env>(_Self&&, _Env&&) noexcept {
          static_assert(sender_expr_for<_Self, when_all_t>);
          return __minvoke<__mtry_catch<__q<__completions>, __q<__error_t>>, _Self, _Env>();
        };

      static constexpr auto get_env = //
        []<class _State, class _Receiver>(
          __ignore,
          _State& __state,
          const _Receiver& __rcvr) noexcept //
        -> __env_t<env_of_t<const _Receiver&>> {
        return __mkenv(stdexec::get_env(__rcvr), __state.__stop_source_);
      };

      static constexpr auto get_state = //
        []<class _Self, class _Receiver>(_Self&& __self, _Receiver& __rcvr)
        -> __sexpr_apply_result_t<_Self, __mk_state_fn_t<env_of_t<_Receiver>>> {
        return __sexpr_apply(
          static_cast<_Self&&>(__self), __when_all::__mk_state_fn(stdexec::get_env(__rcvr)));
      };

      static constexpr auto start = //
        []<class _State, class _Receiver, class... _Operations>(
          _State& __state,
          _Receiver& __rcvr,
          _Operations&... __child_ops) noexcept -> void {
        // register stop callback:
        __state.__on_stop_.emplace(
          get_stop_token(stdexec::get_env(__rcvr)), __on_stop_request{__state.__stop_source_});
        if (__state.__stop_source_.stop_requested()) {
          // Stop has already been requested. Don't bother starting
          // the child operations.
          stdexec::set_stopped(std::move(__rcvr));
        } else {
          (stdexec::start(__child_ops), ...);
          if constexpr (sizeof...(__child_ops) == 0) {
            __state.__complete(__rcvr);
          }
        }
      };

      template <class _State, class _Receiver, class _Error>
      static void __set_error(_State& __state, _Receiver&, _Error&& __err) noexcept {
        // TODO: What memory orderings are actually needed here?
        if (__error != __state.__state_.exchange(__error)) {
          __state.__stop_source_.request_stop();
          // We won the race, free to write the error into the operation
          // state without worry.
          if constexpr (__nothrow_decay_copyable<_Error>) {
            __state.__errors_.template emplace<__decay_t<_Error>>(static_cast<_Error&&>(__err));
          } else {
            try {
              __state.__errors_.template emplace<__decay_t<_Error>>(static_cast<_Error&&>(__err));
            } catch (...) {
              __state.__errors_.template emplace<std::exception_ptr>(std::current_exception());
            }
          }
        }
      }

      static constexpr auto complete = //
        []<class _Index, class _State, class _Receiver, class _Set, class... _Args>(
          _Index,
          _State& __state,
          _Receiver& __rcvr,
          _Set,
          _Args&&... __args) noexcept -> void {
        if constexpr (same_as<_Set, set_error_t>) {
          __set_error(__state, __rcvr, static_cast<_Args&&>(__args)...);
        } else if constexpr (same_as<_Set, set_stopped_t>) {
          __state_t __expected = __started;
          // Transition to the "stopped" state if and only if we're in the
          // "started" state. (If this fails, it's because we're in an
          // error state, which trumps cancellation.)
          if (__state.__state_.compare_exchange_strong(__expected, __stopped)) {
            __state.__stop_source_.request_stop();
          }
        } else if constexpr (!same_as<decltype(_State::__values_), __ignore>) {
          // We only need to bother recording the completion values
          // if we're not already in the "error" or "stopped" state.
          if (__state.__state_ == __started) {
            auto& __opt_values = __tup::__get<__v<_Index>>(__state.__values_);
            using _Tuple = __decayed_custom_tuple<_Args...>;
            static_assert(
              same_as<decltype(*__opt_values), _Tuple&>,
              "One of the senders in this when_all() is fibbing about what types it sends");
            if constexpr ((__nothrow_decay_copyable<_Args> && ...)) {
              __opt_values.emplace(_Tuple{{static_cast<_Args&&>(__args)}...});
            } else {
              try {
                __opt_values.emplace(_Tuple{{static_cast<_Args&&>(__args)}...});
              } catch (...) {
                __set_error(__state, __rcvr, std::current_exception());
              }
            }
          }
        }

        __state.__arrive(__rcvr);
      };
    };

    struct when_all_with_variant_t {
      using _Sender = __1;
      using __legacy_customizations_t = //
        __types<tag_invoke_t(when_all_with_variant_t, _Sender...)>;

      template <sender... _Senders>
        requires __domain::__has_common_domain<_Senders...>
      auto operator()(_Senders&&... __sndrs) const -> __well_formed_sender auto {
        auto __domain = __domain::__common_domain_t<_Senders...>();
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<when_all_with_variant_t>(__(), static_cast<_Senders&&>(__sndrs)...));
      }

      template <class _Sender, class _Env>
      static auto transform_sender(_Sender&& __sndr, const _Env&) {
        // transform the when_all_with_variant into a regular when_all (looking for
        // early when_all customizations), then transform it again to look for
        // late customizations.
        return __sexpr_apply(
          static_cast<_Sender&&>(__sndr),
          [&]<class... _Child>(__ignore, __ignore, _Child&&... __child) {
            return when_all_t()(into_variant(static_cast<_Child&&>(__child))...);
          });
      }
    };

    struct __when_all_with_variant_impl : __sexpr_defaults {
      static constexpr auto get_attrs = //
        []<class... _Child>(__ignore, const _Child&...) noexcept {
          using _Domain = __domain::__common_domain_t<_Child...>;
          if constexpr (same_as<_Domain, default_domain>) {
            return empty_env();
          } else {
            return __env::__with(_Domain(), get_domain);
          }
        };
    };

    struct transfer_when_all_t {
      using _Env = __0;
      using _Sender = __1;
      using __legacy_customizations_t = //
        __types<tag_invoke_t(
          transfer_when_all_t,
          get_completion_scheduler_t<set_value_t>(const _Env&),
          _Sender...)>;

      template <scheduler _Scheduler, sender... _Senders>
        requires __domain::__has_common_domain<_Senders...>
      auto
        operator()(_Scheduler&& __sched, _Senders&&... __sndrs) const -> __well_formed_sender auto {
        using _Env = __t<__schedule_from::__environ<__id<__decay_t<_Scheduler>>>>;
        auto __domain = query_or(get_domain, __sched, default_domain());
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<transfer_when_all_t>(
            _Env{static_cast<_Scheduler&&>(__sched)}, static_cast<_Senders&&>(__sndrs)...));
      }

      template <class _Sender, class _Env>
      static auto transform_sender(_Sender&& __sndr, const _Env&) {
        // transform the transfer_when_all into a regular transform | when_all
        // (looking for early customizations), then transform it again to look for
        // late customizations.
        return __sexpr_apply(
          static_cast<_Sender&&>(__sndr),
          [&]<class _Data, class... _Child>(__ignore, _Data&& __data, _Child&&... __child) {
            return transfer(
              when_all_t()(static_cast<_Child&&>(__child)...),
              get_completion_scheduler<set_value_t>(__data));
          });
      }
    };

    struct __transfer_when_all_impl : __sexpr_defaults {
      static constexpr auto get_attrs = //
        []<class _Data>(const _Data& __data, const auto&...) noexcept -> const _Data& {
        return __data;
      };
    };

    struct transfer_when_all_with_variant_t {
      using _Env = __0;
      using _Sender = __1;
      using __legacy_customizations_t = //
        __types<tag_invoke_t(
          transfer_when_all_with_variant_t,
          get_completion_scheduler_t<set_value_t>(const _Env&),
          _Sender...)>;

      template <scheduler _Scheduler, sender... _Senders>
        requires __domain::__has_common_domain<_Senders...>
      auto
        operator()(_Scheduler&& __sched, _Senders&&... __sndrs) const -> __well_formed_sender auto {
        using _Env = __t<__schedule_from::__environ<__id<__decay_t<_Scheduler>>>>;
        auto __domain = query_or(get_domain, __sched, default_domain());
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<transfer_when_all_with_variant_t>(
            _Env{{static_cast<_Scheduler&&>(__sched)}}, static_cast<_Senders&&>(__sndrs)...));
      }

      template <class _Sender, class _Env>
      static auto transform_sender(_Sender&& __sndr, const _Env&) {
        // transform the transfer_when_all_with_variant into regular transform_when_all
        // and into_variant calls/ (looking for early customizations), then transform it
        // again to look for late customizations.
        return __sexpr_apply(
          static_cast<_Sender&&>(__sndr),
          [&]<class _Data, class... _Child>(__ignore, _Data&& __data, _Child&&... __child) {
            return transfer_when_all_t()(
              get_completion_scheduler<set_value_t>(static_cast<_Data&&>(__data)),
              into_variant(static_cast<_Child&&>(__child))...);
          });
      }
    };

    struct __transfer_when_all_with_variant_impl : __sexpr_defaults {
      static constexpr auto get_attrs = //
        []<class _Data>(const _Data& __data, const auto&...) noexcept -> const _Data& {
        return __data;
      };
    };
  } // namespace __when_all

  using __when_all::when_all_t;
  inline constexpr when_all_t when_all{};

  using __when_all::when_all_with_variant_t;
  inline constexpr when_all_with_variant_t when_all_with_variant{};

  using __when_all::transfer_when_all_t;
  inline constexpr transfer_when_all_t transfer_when_all{};

  using __when_all::transfer_when_all_with_variant_t;
  inline constexpr transfer_when_all_with_variant_t transfer_when_all_with_variant{};

  template <>
  struct __sexpr_impl<when_all_t> : __when_all::__when_all_impl { };

  template <>
  struct __sexpr_impl<when_all_with_variant_t> : __when_all::__when_all_with_variant_impl { };

  template <>
  struct __sexpr_impl<transfer_when_all_t> : __when_all::__transfer_when_all_impl { };

  template <>
  struct __sexpr_impl<transfer_when_all_with_variant_t>
    : __when_all::__transfer_when_all_with_variant_impl { };

  namespace __read {
    template <class _Tag, class _ReceiverId>
    using __result_t = __call_result_t<_Tag, env_of_t<stdexec::__t<_ReceiverId>>>;

    template <class _Tag, class _ReceiverId>
    concept __nothrow_t = __nothrow_callable<_Tag, env_of_t<stdexec::__t<_ReceiverId>>>;

    inline constexpr __mstring __query_failed_diag =
      "The current execution environment doesn't have a value for the given query."_mstr;

    template <class _Tag>
    struct _WITH_QUERY_;

    template <class _Tag, class _Env>
    using __query_failed_error = //
      __mexception<              //
        _NOT_CALLABLE_<"In stdexec::read()..."_mstr, __query_failed_diag>,
        _WITH_QUERY_<_Tag>,
        _WITH_ENVIRONMENT_<_Env>>;

    template <class _Tag, class _Env>
      requires __callable<_Tag, _Env>
    using __completions_t = //
      __if_c<
        __nothrow_callable<_Tag, _Env>,
        completion_signatures<set_value_t(__call_result_t<_Tag, _Env>)>,
        completion_signatures<
          set_value_t(__call_result_t<_Tag, _Env>),
          set_error_t(std::exception_ptr)>>;

    template <class _Tag, class _Ty>
    struct __state {
      using __query = _Tag;
      using __result = _Ty;
      std::optional<_Ty> __result_;
    };

    template <class _Tag, class _Ty>
      requires same_as<_Ty, _Ty&&>
    struct __state<_Tag, _Ty> {
      using __query = _Tag;
      using __result = _Ty;
    };

    struct __read_t {
      template <class _Tag>
      constexpr auto operator()(_Tag) const noexcept {
        return __make_sexpr<__read_t>(_Tag());
      }
    };

    struct __read_impl : __sexpr_defaults {
      using is_dependent = void;

      template <class _Tag, class _Env>
      using __completions_t =
        __minvoke<__mtry_catch_q<__read::__completions_t, __q<__query_failed_error>>, _Tag, _Env>;

      static constexpr auto get_completion_signatures =            //
        []<class _Self, class _Env>(const _Self&, _Env&&) noexcept //
        -> __completions_t<__data_of<_Self>, _Env> {
        static_assert(sender_expr_for<_Self, __read_t>);
        return {};
      };

      static constexpr auto get_state = //
        []<class _Self, class _Receiver>(const _Self&, _Receiver&) noexcept {
          using __query = __data_of<_Self>;
          using __result = __call_result_t<__query, env_of_t<_Receiver>>;
          return __state<__query, __result>();
        };

      static constexpr auto start = //
        []<class _State, class _Receiver>(_State& __state, _Receiver& __rcvr) noexcept -> void {
        using __query = typename _State::__query;
        using __result = typename _State::__result;
        if constexpr (same_as<__result, __result&&>) {
          // The query returns a reference type; pass it straight through to the receiver.
          stdexec::__set_value_invoke(std::move(__rcvr), __query(), stdexec::get_env(__rcvr));
        } else {
          constexpr bool _Nothrow = __nothrow_callable<__query, env_of_t<_Receiver>>;
          auto __query_fn = [&]() noexcept(_Nothrow) -> __result&& {
            __state.__result_.emplace(__conv{[&]() noexcept(_Nothrow) {
              return __query()(stdexec::get_env(__rcvr));
            }});
            return std::move(*__state.__result_);
          };
          stdexec::__set_value_invoke(std::move(__rcvr), __query_fn);
        }
      };
    };
  } // namespace __read

  inline constexpr __read::__read_t read{};

  template <>
  struct __sexpr_impl<__read::__read_t> : __read::__read_impl { };

  namespace __queries {
    template <class _Tag>
    inline auto get_scheduler_t::operator()() const noexcept {
      return read(get_scheduler);
    }

    template <class _Env>
      requires tag_invocable<get_scheduler_t, const _Env&>
    inline auto get_scheduler_t::operator()(const _Env& __env) const noexcept
      -> tag_invoke_result_t<get_scheduler_t, const _Env&> {
      static_assert(nothrow_tag_invocable<get_scheduler_t, const _Env&>);
      static_assert(scheduler<tag_invoke_result_t<get_scheduler_t, const _Env&>>);
      return tag_invoke(get_scheduler_t{}, __env);
    }

    template <class _Tag>
    inline auto get_delegatee_scheduler_t::operator()() const noexcept {
      return read(get_delegatee_scheduler);
    }

    template <class _Env>
      requires tag_invocable<get_delegatee_scheduler_t, const _Env&>
    inline auto get_delegatee_scheduler_t::operator()(const _Env& __t) const noexcept
      -> tag_invoke_result_t<get_delegatee_scheduler_t, const _Env&> {
      static_assert(nothrow_tag_invocable<get_delegatee_scheduler_t, const _Env&>);
      static_assert(scheduler<tag_invoke_result_t<get_delegatee_scheduler_t, const _Env&>>);
      return tag_invoke(get_delegatee_scheduler_t{}, std::as_const(__t));
    }

    template <class _Tag>
    inline auto get_allocator_t::operator()() const noexcept {
      return read(get_allocator);
    }

    template <class _Tag>
    inline auto get_stop_token_t::operator()() const noexcept {
      return read(get_stop_token);
    }

    template <__completion_tag _CPO>
    template <__has_completion_scheduler_for<_CPO> _Queryable>
    auto get_completion_scheduler_t<_CPO>::operator()(const _Queryable& __queryable) const noexcept
      -> tag_invoke_result_t<get_completion_scheduler_t<_CPO>, const _Queryable&> {
      static_assert(
        nothrow_tag_invocable<get_completion_scheduler_t<_CPO>, const _Queryable&>,
        "get_completion_scheduler<_CPO> should be noexcept");
      static_assert(
        scheduler<tag_invoke_result_t<get_completion_scheduler_t<_CPO>, const _Queryable&>>);
      return tag_invoke(*this, __queryable);
    }
  } // namespace __queries

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.adaptors.on]
  namespace __on_v2 {
    inline constexpr __mstring __on_context = "In stdexec::on(Scheduler, Sender)..."_mstr;
    inline constexpr __mstring __no_scheduler_diag =
      "stdexec::on() requires a scheduler to transition back to."_mstr;
    inline constexpr __mstring __no_scheduler_details =
      "The provided environment lacks a value for the get_scheduler() query."_mstr;

    template <
      __mstring _Context = __on_context,
      __mstring _Diagnostic = __no_scheduler_diag,
      __mstring _Details = __no_scheduler_details>
    struct _CANNOT_RESTORE_EXECUTION_CONTEXT_AFTER_ON_ { };

    struct on_t;

    template <class _Sender, class _Env>
    struct __no_scheduler_in_environment {
      using sender_concept = sender_t;

      STDEXEC_MEMFN_DECL(auto get_completion_signatures)(this const __no_scheduler_in_environment&, const auto&) noexcept {
        return __mexception<
          _CANNOT_RESTORE_EXECUTION_CONTEXT_AFTER_ON_<>,
          _WITH_SENDER_<_Sender>,
          _WITH_ENVIRONMENT_<_Env>>{};
      }
    };

    template <class _Scheduler, class _Closure>
    struct __continue_on_data {
      _Scheduler __sched_;
      _Closure __clsur_;
    };
    template <class _Scheduler, class _Closure>
    __continue_on_data(_Scheduler, _Closure) -> __continue_on_data<_Scheduler, _Closure>;

    template <class _Scheduler>
    struct __with_sched {
      _Scheduler __sched_;

      STDEXEC_MEMFN_DECL(auto query)(this const __with_sched& __self, get_scheduler_t) noexcept -> _Scheduler {
        return __self.__sched_;
      }

      STDEXEC_MEMFN_DECL(auto query)(this const __with_sched& __self, get_domain_t) noexcept {
        return query_or(get_domain, __self.__sched_, default_domain());
      }
    };

    template <class _Scheduler>
    __with_sched(_Scheduler) -> __with_sched<_Scheduler>;

    ////////////////////////////////////////////////////////////////////////////////////////////////
    struct on_t {
      template <scheduler _Scheduler, sender _Sender>
      auto operator()(_Scheduler&& __sched, _Sender&& __sndr) const -> __well_formed_sender auto {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<on_t>(static_cast<_Scheduler&&>(__sched), static_cast<_Sender&&>(__sndr)));
      }

      template <sender _Sender, scheduler _Scheduler, __sender_adaptor_closure_for<_Sender> _Closure>
      auto operator()(_Sender&& __sndr, _Scheduler&& __sched, _Closure&& __clsur) const
        -> __well_formed_sender auto {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::transform_sender(
          __domain,
          __make_sexpr<on_t>(
            __continue_on_data{static_cast<_Scheduler&&>(__sched), static_cast<_Closure&&>(__clsur)},
            static_cast<_Sender&&>(__sndr)));
      }

      template <scheduler _Scheduler, __sender_adaptor_closure _Closure>
      STDEXEC_ATTRIBUTE((always_inline))
      auto
        operator()(_Scheduler&& __sched, _Closure&& __clsur) const {
        return __binder_back<on_t, __decay_t<_Scheduler>, __decay_t<_Closure>>{
          {static_cast<_Scheduler&&>(__sched), static_cast<_Closure&&>(__clsur)}
        };
      }

      template <class _Env>
      STDEXEC_ATTRIBUTE((always_inline))
      static auto
        __transform_env_fn(_Env&& __env) noexcept {
        return [&]<class _Data>(__ignore, _Data&& __data, __ignore) noexcept -> decltype(auto) {
          if constexpr (scheduler<_Data>) {
            return __detail::__mkenv_sched(static_cast<_Env&&>(__env), static_cast<_Data&&>(__data));
          } else {
            return static_cast<_Env>(static_cast<_Env&&>(__env));
          }
        };
      }

      template <class _Env>
      STDEXEC_ATTRIBUTE((always_inline))
      static auto
        __transform_sender_fn(const _Env& __env) noexcept {
        return [&]<class _Data, class _Child>(__ignore, _Data&& __data, _Child&& __child) {
          if constexpr (scheduler<_Data>) {
            // This branch handles the case where `on` was called like `on(sch, snd)`
            auto __old = query_or(get_scheduler, __env, __none_such{});
            if constexpr (same_as<decltype(__old), __none_such>) {
              if constexpr (__is_root_env<_Env>) {
                return continue_on(
                  start_on(static_cast<_Data&&>(__data), static_cast<_Child&&>(__child)),
                  std::move(__inln::__scheduler{}));
              } else {
                return __none_such{};
              }
            } else {
              return continue_on(
                start_on(static_cast<_Data&&>(__data), static_cast<_Child&&>(__child)),
                std::move(__old));
            }
          } else {
            // This branch handles the case where `on` was called like `on(snd, sch, clsur)`
            auto __old = query_or(
              get_completion_scheduler<set_value_t>,
              get_env(__child),
              query_or(get_scheduler, __env, __none_such{}));
            if constexpr (same_as<decltype(__old), __none_such>) {
              return __none_such{};
            } else {
              auto&& [__sched, __clsur] = static_cast<_Data&&>(__data);
              return __write(                                                       //
                continue_on(                                                        //
                  __forward_like<_Data>(__clsur)(                                   //
                    continue_on(                                                    //
                      __write(static_cast<_Child&&>(__child), __with_sched{__old}), //
                      __sched)),                                                    //
                  __old),
                __with_sched{__sched});
            }
          }
        };
      }

      template <class _Sender, class _Env>
      STDEXEC_ATTRIBUTE((always_inline))
      static auto
        transform_env(const _Sender& __sndr, _Env&& __env) noexcept {
        return __sexpr_apply(__sndr, __transform_env_fn(static_cast<_Env&&>(__env)));
      }

      template <class _Sender, class _Env>
      STDEXEC_ATTRIBUTE((always_inline))
      static auto
        transform_sender(_Sender&& __sndr, const _Env& __env) {
        auto __tfx_sndr_fn = __transform_sender_fn(__env);
        using _TfxSndrFn = decltype(__tfx_sndr_fn);
        using _NewSndr = __sexpr_apply_result_t<_Sender, _TfxSndrFn>;
        if constexpr (same_as<_NewSndr, __none_such>) {
          return __no_scheduler_in_environment<_Sender, _Env>{};
        } else {
          return __sexpr_apply(
            static_cast<_Sender&&>(__sndr), static_cast<_TfxSndrFn&&>(__tfx_sndr_fn));
        }
      }
    };
  } // namespace __on_v2

  namespace v2 {
    using __on_v2::on_t;
    inline constexpr on_t on{};

    using continue_on_t = v2::on_t;
    inline constexpr continue_on_t continue_on{}; // for back-compat
  }                                               // namespace v2

  template <>
  struct __sexpr_impl<v2::on_t> : __sexpr_defaults {
    using is_dependent = void;
  };

  /////////////////////////////////////////////////////////////////////////////
  // [execution.senders.consumers.sync_wait]
  // [execution.senders.consumers.sync_wait_with_variant]
  namespace __sync_wait {
    inline auto __make_env(run_loop& __loop) noexcept {
      return __env::__with(__loop.get_scheduler(), get_scheduler, get_delegatee_scheduler);
    }

    struct __env : __result_of<__make_env, run_loop&> {
      __env();

      explicit __env(run_loop& __loop) noexcept
        : __result_of<__make_env, run_loop&>{__sync_wait::__make_env(__loop)} {
      }
    };

    // What should sync_wait(just_stopped()) return?
    template <class _Sender, class _Continuation>
    using __sync_wait_result_impl = //
      __try_value_types_of_t<
        _Sender,
        __env,
        __transform<__q<__decay_t>, _Continuation>,
        __q<__msingle>>;

    template <class _Sender>
    using __sync_wait_result_t = __mtry_eval<__sync_wait_result_impl, _Sender, __q<std::tuple>>;

    template <class _Sender>
    using __sync_wait_with_variant_result_t =
      __mtry_eval<__sync_wait_result_impl, __result_of<into_variant, _Sender>, __q<__midentity>>;

    template <class... _Values>
    struct __state {
      using _Tuple = std::tuple<_Values...>;
      std::variant<std::monostate, _Tuple, std::exception_ptr, set_stopped_t> __data_{};
    };

    template <class... _Values>
    struct __receiver {
      struct __t {
        using receiver_concept = receiver_t;
        using __id = __receiver;
        __state<_Values...>* __state_;
        run_loop* __loop_;

        template <class _Error>
        void __set_error(_Error __err) noexcept {
          if constexpr (__decays_to<_Error, std::exception_ptr>)
            __state_->__data_.template emplace<2>(static_cast<_Error&&>(__err));
          else if constexpr (__decays_to<_Error, std::error_code>)
            __state_->__data_.template emplace<2>(std::make_exception_ptr(std::system_error(__err)));
          else
            __state_->__data_.template emplace<2>(
              std::make_exception_ptr(static_cast<_Error&&>(__err)));
          __loop_->finish();
        }

        template <class... _As>
          requires constructible_from<std::tuple<_Values...>, _As...>
        STDEXEC_MEMFN_DECL(
          void set_value)(this __t&& __rcvr, _As&&... __as) noexcept {
          try {
            __rcvr.__state_->__data_.template emplace<1>(static_cast<_As&&>(__as)...);
            __rcvr.__loop_->finish();
          } catch (...) {
            __rcvr.__set_error(std::current_exception());
          }
        }

        template <class _Error>
        STDEXEC_MEMFN_DECL(void set_error)(this __t&& __rcvr, _Error __err) noexcept {
          __rcvr.__set_error(static_cast<_Error&&>(__err));
        }

        STDEXEC_MEMFN_DECL(void set_stopped)(this __t&& __rcvr) noexcept {
          __rcvr.__state_->__data_.template emplace<3>(set_stopped_t{});
          __rcvr.__loop_->finish();
        }

        STDEXEC_MEMFN_DECL(auto get_env)(this const __t& __rcvr) noexcept -> __env {
          return __env(*__rcvr.__loop_);
        }
      };
    };

    template <class _Sender>
    using __receiver_t = __t<__sync_wait_result_impl<_Sender, __q<__receiver>>>;

    // These are for hiding the metaprogramming in diagnostics
    template <class _Sender>
    struct __sync_receiver_for {
      using __t = __receiver_t<_Sender>;
    };
    template <class _Sender>
    using __sync_receiver_for_t = __t<__sync_receiver_for<_Sender>>;

    template <class _Sender>
    struct __value_tuple_for {
      using __t = __sync_wait_result_t<_Sender>;
    };
    template <class _Sender>
    using __value_tuple_for_t = __t<__value_tuple_for<_Sender>>;

    template <class _Sender>
    struct __variant_for {
      using __t = __sync_wait_with_variant_result_t<_Sender>;
    };
    template <class _Sender>
    using __variant_for_t = __t<__variant_for<_Sender>>;

    inline constexpr __mstring __sync_wait_context_diag = //
      "In stdexec::sync_wait()..."_mstr;
    inline constexpr __mstring __too_many_successful_completions_diag =
      "The argument to stdexec::sync_wait() is a sender that can complete successfully in more "
      "than one way. Use stdexec::sync_wait_with_variant() instead."_mstr;

    template <__mstring _Context, __mstring _Diagnostic>
    struct _INVALID_ARGUMENT_TO_SYNC_WAIT_;

    template <__mstring _Diagnostic>
    using __invalid_argument_to_sync_wait =
      _INVALID_ARGUMENT_TO_SYNC_WAIT_<__sync_wait_context_diag, _Diagnostic>;

    template <__mstring _Diagnostic, class _Sender, class _Env = __env>
    using __sync_wait_error = __mexception<
      __invalid_argument_to_sync_wait<_Diagnostic>,
      _WITH_SENDER_<_Sender>,
      _WITH_ENVIRONMENT_<_Env>>;

    template <class _Sender, class>
    using __too_many_successful_completions_error =
      __sync_wait_error<__too_many_successful_completions_diag, _Sender>;

    template <class _Sender>
    concept __valid_sync_wait_argument = __ok<__minvoke<
      __mtry_catch_q<__single_value_variant_sender_t, __q<__too_many_successful_completions_error>>,
      _Sender,
      __env>>;

#if STDEXEC_NVHPC()
    // It requires some hoop-jumping to get the NVHPC compiler to report a meaningful
    // diagnostic for SFINAE failures.
    template <class _Sender>
    auto __diagnose_error() {
      if constexpr (!sender_in<_Sender, __env>) {
        using _Completions = __completion_signatures_of_t<_Sender, __env>;
        if constexpr (__merror<_Completions>) {
          return _Completions();
        } else {
          constexpr __mstring __diag =
            "The stdexec::sender_in<Sender, Environment> concept check has failed."_mstr;
          return __sync_wait_error<__diag, _Sender>();
        }
      } else if constexpr (!__valid_sync_wait_argument<_Sender>) {
        return __sync_wait_error<__too_many_successful_completions_diag, _Sender>();
      } else if constexpr (!sender_to<_Sender, __sync_receiver_for_t<_Sender>>) {
        constexpr __mstring __diag =
          "Failed to connect the given sender to sync_wait's internal receiver. "
          "The stdexec::connect(Sender, Receiver) expression is ill-formed."_mstr;
        return __sync_wait_error<__diag, _Sender>();
      } else {
        constexpr __mstring __diag = "Unknown concept check failure."_mstr;
        return __sync_wait_error<__diag, _Sender>();
      }
    }

    template <class _Sender>
    using __error_description_t = decltype(__sync_wait::__diagnose_error<_Sender>());
#endif

    ////////////////////////////////////////////////////////////////////////////
    // [execution.senders.consumers.sync_wait]
    struct sync_wait_t {
      template <sender_in<__env> _Sender>
        requires __valid_sync_wait_argument<_Sender>
              && __has_implementation_for<sync_wait_t, __early_domain_of_t<_Sender>, _Sender>
      auto operator()(_Sender&& __sndr) const -> std::optional<__value_tuple_for_t<_Sender>> {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::apply_sender(__domain, *this, static_cast<_Sender&&>(__sndr));
      }

#if STDEXEC_NVHPC()
      // This is needed to get sensible diagnostics from nvc++
      template <class _Sender, class _Error = __error_description_t<_Sender>>
      auto operator()(_Sender&&, [[maybe_unused]] _Error __diagnostic = {}) const
        -> std::optional<std::tuple<int>> = delete;
#endif

      using _Sender = __0;
      using __legacy_customizations_t = __types<
        // For legacy reasons:
        tag_invoke_t(
          sync_wait_t,
          get_completion_scheduler_t<set_value_t>(get_env_t(const _Sender&)),
          _Sender),
        tag_invoke_t(sync_wait_t, _Sender)>;

      // clang-format off
      /// @brief Synchronously wait for the result of a sender, blocking the
      ///         current thread.
      ///
      /// `sync_wait` connects and starts the given sender, and then drives a
      ///         `run_loop` instance until the sender completes. Additional work
      ///         can be delegated to the `run_loop` by scheduling work on the
      ///         scheduler returned by calling `get_delegatee_scheduler` on the
      ///         receiver's environment.
      ///
      /// @pre The sender must have a exactly one value completion signature. That
      ///         is, it can only complete successfully in one way, with a single
      ///         set of values.
      ///
      /// @retval success Returns an engaged `std::optional` containing the result
      ///         values in a `std::tuple`.
      /// @retval canceled Returns an empty `std::optional`.
      /// @retval error Throws the error.
      ///
      /// @throws std::rethrow_exception(error) if the error has type
      ///         `std::exception_ptr`.
      /// @throws std::system_error(error) if the error has type
      ///         `std::error_code`.
      /// @throws error otherwise
      // clang-format on
      template <class _Sender>
        requires sender_to<_Sender, __sync_receiver_for_t<_Sender>>
      auto apply_sender(_Sender&& __sndr) const -> std::optional<__sync_wait_result_t<_Sender>> {
        using state_t = __sync_wait_result_impl<_Sender, __q<__state>>;
        state_t __state{};
        run_loop __loop;

        // Launch the sender with a continuation that will fill in a variant
        // and notify a condition variable.
        auto __op_state =
          connect(static_cast<_Sender&&>(__sndr), __receiver_t<_Sender>{&__state, &__loop});
        start(__op_state);

        // Wait for the variant to be filled in.
        __loop.run();

        if (__state.__data_.index() == 2)
          std::rethrow_exception(std::get<2>(__state.__data_));

        if (__state.__data_.index() == 3)
          return std::nullopt;

        return std::move(std::get<1>(__state.__data_));
      }
    };

    ////////////////////////////////////////////////////////////////////////////
    // [execution.senders.consumers.sync_wait_with_variant]
    struct sync_wait_with_variant_t {
      struct __impl;

      template <sender_in<__env> _Sender>
        requires __callable<
          apply_sender_t,
          __early_domain_of_t<_Sender>,
          sync_wait_with_variant_t,
          _Sender>
      auto operator()(_Sender&& __sndr) const -> std::optional<__variant_for_t<_Sender>> {
        auto __domain = __get_early_domain(__sndr);
        return stdexec::apply_sender(__domain, *this, static_cast<_Sender&&>(__sndr));
      }

#if STDEXEC_NVHPC()
      template <
        class _Sender,
        class _Error = __error_description_t<__result_of<into_variant, _Sender>>>
      auto operator()(_Sender&&, [[maybe_unused]] _Error __diagnostic = {}) const
        -> std::optional<std::tuple<std::variant<std::tuple<>>>> = delete;
#endif

      using _Sender = __0;
      using __legacy_customizations_t = __types<
        // For legacy reasons:
        tag_invoke_t(
          sync_wait_with_variant_t,
          get_completion_scheduler_t<set_value_t>(get_env_t(const _Sender&)),
          _Sender),
        tag_invoke_t(sync_wait_with_variant_t, _Sender)>;

      template <class _Sender>
        requires __callable<sync_wait_t, __result_of<into_variant, _Sender>>
      auto apply_sender(_Sender&& __sndr) const -> std::optional<__variant_for_t<_Sender>> {
        if (auto __opt_values = sync_wait_t()(into_variant(static_cast<_Sender&&>(__sndr)))) {
          return std::move(std::get<0>(*__opt_values));
        }
        return std::nullopt;
      }
    };
  } // namespace __sync_wait

  using __sync_wait::sync_wait_t;
  inline constexpr sync_wait_t sync_wait{};

  using __sync_wait::sync_wait_with_variant_t;
  inline constexpr sync_wait_with_variant_t sync_wait_with_variant{};

  //////////////////////////////////////////////////////////////////////////////////////////////////
  struct __ignore_sender {
    using sender_concept = sender_t;

    template <sender _Sender>
    constexpr __ignore_sender(_Sender&&) noexcept {
    }
  };

  template <auto _Reason = "You cannot pipe one sender into another."_mstr>
  struct _CANNOT_PIPE_INTO_A_SENDER_ { };

  template <class _Sender>
  using __bad_pipe_sink_t = __mexception<_CANNOT_PIPE_INTO_A_SENDER_<>, _WITH_SENDER_<_Sender>>;
} // namespace stdexec

#if STDEXEC_MSVC()
namespace stdexec {
  // MSVCBUG https://developercommunity.visualstudio.com/t/Incorrect-codegen-in-await_suspend-aroun/10454102

  // MSVC incorrectly allocates the return buffer for await_suspend calls within the suspended coroutine
  // frame. When the suspended coroutine is destroyed within await_suspend, the continuation coroutine handle
  // is not only used after free, but also overwritten by the debug malloc implementation when NRVO is in play.

  // This workaround delays the destruction of the suspended coroutine by wrapping the continuation in another
  // coroutine which destroys the former and transfers execution to the original continuation.

  // The wrapping coroutine is thread-local and is reused within the thread for each destroy-and-continue sequence.
  // The wrapping coroutine itself is destroyed at thread exit.

  namespace __destroy_and_continue_msvc {
    struct __task {
      struct promise_type {
        __task get_return_object() noexcept {
          return {__coro::coroutine_handle<promise_type>::from_promise(*this)};
        }

        static std::suspend_never initial_suspend() noexcept {
          return {};
        }

        static std::suspend_never final_suspend() noexcept {
          STDEXEC_ASSERT(!"Should never get here");
          return {};
        }

        static void return_void() noexcept {
          STDEXEC_ASSERT(!"Should never get here");
        }

        static void unhandled_exception() noexcept {
          STDEXEC_ASSERT(!"Should never get here");
        }
      };

      __coro::coroutine_handle<> __coro_;
    };

    struct __continue_t {
      static constexpr bool await_ready() noexcept {
        return false;
      }

      __coro::coroutine_handle<> await_suspend(__coro::coroutine_handle<>) noexcept {
        return __continue_;
      }

      static void await_resume() noexcept {
      }

      __coro::coroutine_handle<> __continue_;
    };

    struct __context {
      __coro::coroutine_handle<> __destroy_;
      __coro::coroutine_handle<> __continue_;
    };

    inline __task __co_impl(__context& __c) {
      while (true) {
        co_await __continue_t{__c.__continue_};
        __c.__destroy_.destroy();
      }
    }

    struct __context_and_coro {
      __context_and_coro() {
        __context_.__continue_ = __coro::noop_coroutine();
        __coro_ = __co_impl(__context_).__coro_;
      }

      ~__context_and_coro() {
        __coro_.destroy();
      }

      __context __context_;
      __coro::coroutine_handle<> __coro_;
    };

    inline __coro::coroutine_handle<>
      __impl(__coro::coroutine_handle<> __destroy, __coro::coroutine_handle<> __continue) {
      static thread_local __context_and_coro __c;
      __c.__context_.__destroy_ = __destroy;
      __c.__context_.__continue_ = __continue;
      return __c.__coro_;
    }
  } // namespace __destroy_and_continue_msvc
} // namespace stdexec

#  define STDEXEC_DESTROY_AND_CONTINUE(__destroy, __continue)                                      \
    (::stdexec::__destroy_and_continue_msvc::__impl(__destroy, __continue))
#else
#  define STDEXEC_DESTROY_AND_CONTINUE(__destroy, __continue) (__destroy.destroy(), __continue)
#endif

// For issuing a meaningful diagnostic for the erroneous `snd1 | snd2`.
template <stdexec::sender _Sender>
  requires stdexec::__ok<stdexec::__bad_pipe_sink_t<_Sender>>
auto operator|(stdexec::__ignore_sender, _Sender&&) noexcept -> stdexec::__ignore_sender;

#include "__detail/__p2300.hpp"

STDEXEC_PRAGMA_POP()
