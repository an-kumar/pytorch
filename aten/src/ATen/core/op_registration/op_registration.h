#pragma once

/**
 * Include this file if you want to register operators. It includes all
 * functionality needed to do so for you.
 */

#include <c10/core/DispatchKey.h>
#include <ATen/core/dispatch/Dispatcher.h>
#include <ATen/core/op_registration/infer_schema.h>
#if defined(EXPOSE_C2_OPS) || !defined(CAFFE2_IS_XPLAT_BUILD)
#include <torch/csrc/jit/frontend/function_schema_parser.h>
#endif
#include <ATen/core/ATenOpList.h>

namespace c10 {

namespace detail {
template<class KernelFunctor>
std::unique_ptr<FunctionSchema> inferFunctionSchemaFromFunctor() {
  using func_type = typename c10::guts::infer_function_traits_t<KernelFunctor>::func_type;
  return std::make_unique<FunctionSchema>(inferFunctionSchemaFlattenedReturns<func_type>("", ""));
}
}

/**
 * An instance of this class handles the registration for one or more operators.
 * Make sure you keep the RegisterOperators instance around since it will
 * deregister the operator it's responsible for in its destructor.
 *
 * Example:
 *
 * > namespace {
 * >   class my_kernel_cpu final : public c10::OperatorKernel {
 * >   public:
 * >     Tensor operator()(Tensor a, Tensor b) {...}
 * >   };
 * > }
 * >
 * > static auto registry = c10::RegisterOperators()
 * >     .op(c10::RegisterOperators::options()
 * >         .schema("my_op")
 * >         .kernel<my_kernel_cpu>(DispatchKey::CPU));
 */
class CAFFE2_API RegisterOperators final {
public:
  RegisterOperators();
  ~RegisterOperators();

  RegisterOperators(const RegisterOperators&) = delete;
  RegisterOperators& operator=(const RegisterOperators&) = delete;
  RegisterOperators(RegisterOperators&&) noexcept;
  RegisterOperators& operator=(RegisterOperators&&) noexcept;

  class CAFFE2_API Options final {
  public:
    Options(const Options&) = delete;
    Options(Options&&) noexcept = delete;
    Options& operator=(const Options&) = delete;
    Options& operator=(Options&&) noexcept = delete;

    // internal-only for registering stack based kernels
    template<KernelFunction::BoxedKernelFunction* kernel_func>
    Options&& kernel(DispatchKey dispatch_key) && {
      return std::move(*this).kernel(dispatch_key, KernelFunction::makeFromBoxedFunction<kernel_func>(), nullptr);
    }

    // internal-only for registering stack based catch-all kernels
    template<KernelFunction::BoxedKernelFunction* kernel_func>
    Options&& catchAllKernel() && {
      return std::move(*this).kernel(c10::nullopt, KernelFunction::makeFromBoxedFunction<kernel_func>(), nullptr);
    }

    // internal only for registering caffe2 ops
    Options&& schema(FunctionSchema&& schema) {
        TORCH_CHECK(!schemaOrName_.has_value(), "You can only specify the schema once per operator registration.");
        schemaOrName_ = c10::make_right<OperatorName, FunctionSchema>(std::move(schema));
        return std::move(*this);
    }

    /**
     * Use this to specify the schema for an operator. You can also specify
     * the operator name only to have the function signature part of the
     * schema be inferred from the kernel function.
     *
     * Example:
     *
     * > // Infer function signature from my_kernel_cpu
     * > static auto registry = c10::RegisterOperators()
     * >     .op(c10::RegisterOperators::options()
     * >         .schema("my_op")
     * >         .kernel<my_kernel_cpu>(DispatchKey::CPU));
     * >
     * >
     * > // Explicitly specify full schema
     * > static auto registry = c10::RegisterOperators()
     * >     .op(c10::RegisterOperators::options()
     * >         .schema("my_op(Tensor a) -> Tensor")
     * >         .kernel<my_kernel_cpu>(DispatchKey::CPU));
     */
    Options&& schema(const std::string& schemaOrName) {
      TORCH_CHECK(!schemaOrName_.has_value(), "Tried to register operator ", schemaOrName," but specified schema multiple times. You can only specify the schema once per operator registration.");

      #if !defined(EXPOSE_C2_OPS) && defined(CAFFE2_IS_XPLAT_BUILD)
        throw std::logic_error("Tried to register operator " + schemaOrName + ". We don't support registering c10 ops on mobile yet because the function schema parser isn't present in the mobile build.");
      #else
        schemaOrName_ = torch::jit::parseSchemaOrName(schemaOrName);
      #endif

      return std::move(*this);
    }

    /**
     * Use this to register an operator whose kernel is implemented as a functor.
     * The kernel is only called for inputs matching the given dispatch key.
     * You can register multiple kernels for different dispatch keys.
     *
     * Example:
     *
     * > namespace {
     * >   class my_kernel_cpu final : public c10::OperatorKernel {
     * >   public:
     * >     Tensor operator()(Tensor a, Tensor b) {...}
     * >   };
     * > }
     * >
     * > static auto registry = c10::RegisterOperators()
     * >     .op(c10::RegisterOperators::options()
     * >         .schema("my_op")
     * >         .kernel<my_kernel_cpu>(DispatchKey::CPU));
     *
     * The functor constructor can take arguments to configure the kernel.
     * The arguments are defined in the kernel registration.
     * Example:
     *
     * > namespace {
     * >   class my_kernel_cpu final : public c10::OperatorKernel {
     * >   public:
     * >     explicit my_kernel_cpu(std::string some_configuration, int a, bool b)
     * >         : ... {...}
     * >
     * >     Tensor operator()(Tensor a, Tensor b) {...}
     * >   };
     * > }
     * >
     * > static auto registry = c10::RegisterOperators()
     * >     .op(c10::RegisterOperators::options()
     * >         .schema("my_op")
     * >         .kernel<my_kernel_cpu>(DispatchKey::CPU, "some_configuration", 3, true));
     */
    template<class KernelFunctor, class... ConstructorParameters>
    // enable_if: only enable it if KernelFunctor is actually a functor
    std::enable_if_t<guts::is_functor<KernelFunctor>::value, Options&&> kernel(DispatchKey dispatch_key, ConstructorParameters&&... constructorParameters) && {
      static_assert(std::is_base_of<OperatorKernel, KernelFunctor>::value, "Tried to register a kernel functor using the kernel<Functor>() API, but it doesn't inherit from c10::OperatorKernel. Please have the functor inherit from it.");
      static_assert(std::is_constructible<KernelFunctor, ConstructorParameters...>::value, "Wrong argument list for constructor of kernel functor. The arguments to kernel<Functor>(arguments...) must match one of the constructors of Functor.");

      return std::move(*this).kernel(
        std::move(dispatch_key),
        KernelFunction::makeFromUnboxedFunctor<false, KernelFunctor>(std::make_unique<KernelFunctor>(std::forward<ConstructorParameters>(constructorParameters)...)),
        detail::inferFunctionSchemaFromFunctor<KernelFunctor>()
      );
    }

    /**
     * Use this to register an operator whose kernel is implemented as a functor.
     * The kernel is a catch-all kernel, meaning it's called independent from
     * the input. Dispatch is disabled for this operator.
     *
     * Example:
     *
     * > namespace {
     * >   class my_kernel_cpu final : public c10::OperatorKernel {
     * >   public:
     * >     Tensor operator()(Tensor a, Tensor b) {...}
     * >   };
     * > }
     * >
     * > static auto registry = c10::RegisterOperators()
     * >     .op(c10::RegisterOperators::options()
     * >         .schema("my_op")
     * >         .catchAllKernel<my_kernel_cpu>());
     *
     * The functor constructor can take arguments to configure the kernel.
     * The arguments are defined in the kernel registration.
     * Example:
     *
     * > namespace {
     * >   class my_kernel_cpu final : public c10::OperatorKernel {
     * >   public:
     * >     explicit my_kernel_cpu(std::string some_configuration, int a, bool b)
     * >         : ... {...}
     * >
     * >     Tensor operator()(Tensor a, Tensor b) {...}
     * >   };
     * > }
     * >
     * > static auto registry = c10::RegisterOperators()
     * >     .op(c10::RegisterOperators::options()
     * >         .schema("my_op")
     * >         .catchAllKernel<my_kernel_cpu>("some_configuration", 3, true));
     */
    template<class KernelFunctor, class... ConstructorParameters>
    // enable_if: only enable it if KernelFunctor is actually a functor
    std::enable_if_t<guts::is_functor<KernelFunctor>::value, Options&&> catchAllKernel(ConstructorParameters&&... constructorParameters) && {
      static_assert(std::is_base_of<OperatorKernel, KernelFunctor>::value, "Tried to register a kernel functor using the kernel<Functor>() API, but it doesn't inherit from c10::OperatorKernel. Please have the functor inherit from it.");
      static_assert(std::is_constructible<KernelFunctor, ConstructorParameters...>::value, "Wrong argument list for constructor of kernel functor. The arguments to kernel<Functor>(arguments...) must match one of the constructors of Functor.");

      return std::move(*this).kernel(
        c10::nullopt,
        KernelFunction::makeFromUnboxedFunctor<false, KernelFunctor>(std::make_unique<KernelFunctor>(std::forward<ConstructorParameters>(constructorParameters)...)),
        detail::inferFunctionSchemaFromFunctor<KernelFunctor>()
      );
    }

    /**
     * Use this to register an operator whose kernel is implemented by a function.
     * The kernel is only called for inputs matching the given dispatch key.
     * You can register multiple kernels for different dispatch keys.
     *
     * Example:
     *
     * > namespace { Tensor my_kernel_cpu(Tensor a, Tensor b) {...} }
     * >
     * > static auto registry = c10::RegisterOperators()
     * >     .op(c10::RegisterOperators::options()
     * >         .schema("my_op")
     * >         .kernel<decltype(my_kernel_cpu), &my_kernel_cpu>(DispatchKey::CPU));
     */
    template<class FuncType, FuncType* kernel_func>
    // enable_if: only enable it if FuncType is actually a function
    std::enable_if_t<guts::is_function_type<FuncType>::value, Options&&> kernel(DispatchKey dispatch_key) && {
      static_assert(!std::is_same<FuncType, KernelFunction::BoxedKernelFunction>::value, "Tried to register a stackbased (i.e. internal) kernel function using the public kernel<...>() API. Please either use the internal kernel(...) API or also implement the kernel function as defined by the public API.");
      static_assert(kernel_func != nullptr, "Kernel function cannot be nullptr");

      return std::move(*this).kernel(
        std::move(dispatch_key),
        KernelFunction::makeFromUnboxedFunction<FuncType, kernel_func>(),
        // TODO Do schema inference without relying on WrapFunctionIntoFunctor
        detail::inferFunctionSchemaFromFunctor<typename impl::WrapFunctionIntoFunctor<FuncType, kernel_func>::type>()
      );
    }

    /**
     * Use this to register an operator whose kernel is implemented by a function.
     * The kernel is a catch-all kernel, meaning it's called independent from
     * the input. Dispatch is disabled for this operator.
     *
     * Example:
     *
     * > namespace { Tensor my_kernel_cpu(Tensor a, Tensor b) {...} }
     * >
     * > static auto registry = c10::RegisterOperators()
     * >     .op(c10::RegisterOperators::options()
     * >         .schema("my_op")
     * >         .catchAllKernel<decltype(my_kernel_cpu), &my_kernel_cpu>());
     */
    template<class FuncType, FuncType* kernel_func>
    // enable_if: only enable it if FuncType is actually a function
    std::enable_if_t<guts::is_function_type<FuncType>::value, Options&&> catchAllKernel() && {
      static_assert(!std::is_same<FuncType, KernelFunction::BoxedKernelFunction>::value, "Tried to register a stackbased (i.e. internal) kernel function using the public kernel<...>() API. Please either use the internal kernel(...) API or also implement the kernel function as defined by the public API.");
      static_assert(kernel_func != nullptr, "Kernel function cannot be nullptr");

      return std::move(*this).kernel(
        c10::nullopt,
        KernelFunction::makeFromUnboxedFunction<FuncType, kernel_func>(),
        // TODO Do schema inference without relying on WrapFunctionIntoFunctor
        detail::inferFunctionSchemaFromFunctor<typename impl::WrapFunctionIntoFunctor<FuncType, kernel_func>::type>()
      );
    }

    template<class FuncType>
    // enable_if: only enable it if FuncType is actually a function
    std::enable_if_t<guts::is_function_type<FuncType>::value, Options&&> kernel(DispatchKey dispatch_key, FuncType* kernel_func) && {
      static_assert(!std::is_same<FuncType, KernelFunction::BoxedKernelFunction>::value, "Tried to register a stackbased (i.e. internal) kernel function using the public kernel<...>() API. Please either use the internal kernel(...) API or also implement the kernel function as defined by the public API.");
      TORCH_INTERNAL_ASSERT(kernel_func != nullptr, "Kernel function cannot be nullptr");

      return std::move(*this).kernel(
        std::move(dispatch_key),
        KernelFunction::makeFromUnboxedRuntimeFunction(kernel_func),
        // TODO Do schema inference without relying on WrapFunctionIntoFunctor
        detail::inferFunctionSchemaFromFunctor<impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<FuncType>>>()
      );
    }

    template<class FuncType>
    // enable_if: only enable it if FuncType is actually a function
    std::enable_if_t<guts::is_function_type<FuncType>::value, Options&&> catchAllKernel(FuncType* kernel_func) && {
      static_assert(!std::is_same<FuncType, KernelFunction::BoxedKernelFunction>::value, "Tried to register a stackbased (i.e. internal) kernel function using the public kernel<...>() API. Please either use the internal kernel(...) API or also implement the kernel function as defined by the public API.");
      TORCH_INTERNAL_ASSERT(kernel_func != nullptr, "Kernel function cannot be nullptr");

      return std::move(*this).kernel(
        c10::nullopt,
        KernelFunction::makeFromUnboxedRuntimeFunction(kernel_func),
        // TODO Do schema inference without relying on WrapFunctionIntoFunctor
        detail::inferFunctionSchemaFromFunctor<impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<FuncType>>>()
      );
    }

    // TODO Remove impl_unboxedOnlyKernel once all of aten can generate boxed kernels
    template<class FuncType, FuncType* kernel_func>
    // enable_if: only enable it if FuncType is actually a function
    std::enable_if_t<guts::is_function_type<FuncType>::value, Options&&> impl_unboxedOnlyKernel(DispatchKey dispatch_key) && {
      static_assert(!std::is_same<FuncType, KernelFunction::BoxedKernelFunction>::value, "Tried to register a stackbased (i.e. internal) kernel function using the public kernel<...>() API. Please either use the internal kernel(...) API or also implement the kernel function as defined by the public API.");
      static_assert(kernel_func != nullptr, "Kernel function cannot be nullptr");

      return std::move(*this).kernel(
        std::move(dispatch_key),
        KernelFunction::makeFromUnboxedOnlyRuntimeFunction(kernel_func),
        nullptr // disable function schema inference because some ops from native_functions.yaml don't support it yet
      );
    }

    // TODO Remove impl_unboxedOnlyCatchAllKernel once all of aten can generate boxed kernels
    template<class FuncType, FuncType* kernel_func>
    // enable_if: only enable it if FuncType is actually a function
    std::enable_if_t<guts::is_function_type<FuncType>::value, Options&&> impl_unboxedOnlyCatchAllKernel() && {
      static_assert(!std::is_same<FuncType, KernelFunction::BoxedKernelFunction>::value, "Tried to register a stackbased (i.e. internal) kernel function using the public kernel<...>() API. Please either use the internal kernel(...) API or also implement the kernel function as defined by the public API.");
      static_assert(kernel_func != nullptr, "Kernel function cannot be nullptr");

      return std::move(*this).kernel(
        c10::nullopt,
        KernelFunction::makeFromUnboxedOnlyRuntimeFunction(kernel_func),
        nullptr // disable function schema inference because some ops from native_functions.yaml don't support it yet
      );
    }

    /**
     * Use this to register an operator whose kernel is implemented as a lambda.
     * The kernel is only called for inputs matching the given dispatch key.
     * You can register multiple kernels for different dispatch keys.
     *
     * The lambda must be stateless, i.e. not have a capture. If your kernel
     * needs to store some configuration parameters, write the kernel as a
     * functor instead.
     *
     * Example:
     *
     * > static auto registry = c10::RegisterOperators()
     * >     .op(c10::RegisterOperators::options()
     * >         .schema("my_op")
     * >         .kernel(DispatchKey::CPU, [] (Tensor a) -> Tensor {...}));
     */
    template<class Lambda>
    // enable_if: only enable it if Lambda is a functor (note: lambdas are functors)
    std::enable_if_t<
        guts::is_functor<std::decay_t<Lambda>>::value
        && !std::is_same<typename guts::infer_function_traits_t<std::decay_t<Lambda>>::func_type, KernelFunction::BoxedKernelFunction>::value,
        Options&&> kernel(DispatchKey dispatch_key, Lambda&& functor) && {
      static_assert(!std::is_base_of<OperatorKernel, std::decay_t<Lambda>>::value, "The kernel(x) API for registering a kernel is only meant to be used with lambdas. Your kernel is a functor. Please use the kernel<Functor>() API instead.");

      // We don't support stateful lambdas (i.e. lambdas with a capture), because their
      // behavior would be nonobvious. A functor kernel with cache gets a new instance of
      // its cache each time the kernel is looked up from the dispatch table.
      // A lambda with a capture would be global and share its capture between all kernel lookups.
      // So, instead of making users having to think about it (including the thread-safety
      // issues this causes), let's just forbid stateful lambdas altogether.
      static_assert(guts::is_stateless_lambda<std::decay_t<Lambda>>::value, "The kernel(x) API for registering a kernel only works for stateless lambdas (i.e. lambdas without captures). If you need a cache, please use the functor based API kernel<Functor>() instead.");

      return std::move(*this).kernel(
        std::move(dispatch_key),
        KernelFunction::makeFromUnboxedLambda(std::forward<Lambda>(functor)),
        // TODO Do schema inference without relying on WrapFunctionIntoRuntimeFunctor
        detail::inferFunctionSchemaFromFunctor<impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<Lambda>>>()
      );
    }

    /**
     * Use this to register an operator whose kernel is implemented as a lambda.
     * The kernel is a catch-all kernel, meaning it's called independent from
     * the input. Dispatch is disabled for this operator.
     *
     * The lambda must be stateless, i.e. not have a capture. If your kernel
     * needs to store some configuration parameters, write the kernel as a
     * functor instead.
     *
     * Example:
     *
     * > static auto registry = c10::RegisterOperators()
     * >     .op(c10::RegisterOperators::options()
     * >         .schema("my_op")
     * >         .catchAllKernel([] (Tensor a) -> Tensor {...}));
     */
    template<class Lambda>
    // enable_if: only enable it if Lambda is a functor (note: lambdas are functors)
    std::enable_if_t<
        guts::is_functor<std::decay_t<Lambda>>::value
        && !std::is_same<typename guts::infer_function_traits_t<std::decay_t<Lambda>>::func_type, KernelFunction::BoxedKernelFunction>::value,
        Options&&> catchAllKernel(Lambda&& lambda) && {
      static_assert(!std::is_base_of<OperatorKernel, std::decay_t<Lambda>>::value, "The kernel(x) API for registering a kernel is only meant to be used with lambdas. Your kernel is a functor. Please use the kernel<Functor>() API instead.");

      // We don't support stateful lambdas (i.e. lambdas with a capture), because their
      // behavior would be nonobvious.
      // A lambda with a capture would be global and share its capture between all kernel lookups.
      // This would be a likely source for unexpected race conditions, so we forbid it.
      // If a kernel really needs global state, they can just have regular global state
      // in their .cpp file next to the kernel lambda.
      static_assert(guts::is_stateless_lambda<std::decay_t<Lambda>>::value, "The kernel(x) API for registering a kernel only works for stateless lambdas (i.e. lambdas without captures). If you need a cache, please use the functor based API kernel<Functor>() instead.");

      return std::move(*this).kernel(
        c10::nullopt,
        KernelFunction::makeFromUnboxedLambda(std::forward<Lambda>(lambda)),
        // TODO Do schema inference without relying on WrapFunctionIntoRuntimeFunctor
        detail::inferFunctionSchemaFromFunctor<impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<Lambda>>>()
      );
    }

    Options&& aliasAnalysis(AliasAnalysisKind aliasAnalysisKind) && {
      TORCH_CHECK(!aliasAnalysisKind_.has_value(), "You can only call aliasAnalysis() once per operator registration.");
      aliasAnalysisKind_ = aliasAnalysisKind;
      return std::move(*this);
    }

  private:
    Options&& kernel(c10::optional<DispatchKey> dispatch_key, KernelFunction&& func, std::unique_ptr<FunctionSchema>&& inferred_function_schema) && {
      KernelRegistrationConfig config;
      config.dispatch_key = dispatch_key;
      config.func = std::move(func);
      config.inferred_function_schema = std::move(inferred_function_schema);
      kernels.push_back(std::move(config));
      return std::move(*this);
    }

    Options()
    : schemaOrName_(c10::nullopt)
    , kernels()
    , aliasAnalysisKind_(c10::nullopt)
    {}

    // KernelRegistrationConfig accumulates all information from the config
    // parameters passed to a RegisterOperators::op() call into one object.
    struct KernelRegistrationConfig final {
      KernelRegistrationConfig()
        : dispatch_key(c10::nullopt)
        , func()
        , inferred_function_schema(nullptr)
      {}

      c10::optional<DispatchKey> dispatch_key;
      KernelFunction func;
      std::unique_ptr<FunctionSchema> inferred_function_schema;
    };

    c10::optional<c10::either<OperatorName, FunctionSchema>> schemaOrName_;

    std::vector<KernelRegistrationConfig> kernels;
    optional<AliasAnalysisKind> aliasAnalysisKind_;
    friend class RegisterOperators;
    friend class Library;
  };

  /**
   * Call this to get an instance of registration options, which
   * can be passed to a call to RegisterOperators::op() to specify
   * these options for the operator registration.
   * See class doc comment for examples.
   */
  static Options options() {
    return {};
  }

  /**
   * Call this to register an operator. See class doc comment for examples.
   */
  RegisterOperators&& op(Options&& options) && {
    checkSchemaAndRegisterOp_(std::move(options));
    return std::move(*this);
  }

  // Regular mutator version of the && version above
  RegisterOperators& op(Options&& options) & {
    checkSchemaAndRegisterOp_(std::move(options));
    return *this;
  }

  /**
   * This is a shorthand for RegisterOperators::op(Options) where you can
   * specify the operator schema outside of the options parameter.
   * See class doc comment for examples.
   */
  RegisterOperators&& op(const std::string& schemaOrName, Options&& options = RegisterOperators::options()) && {
    return std::move(*this).op(std::move(options).schema(schemaOrName));
  }

  // internal only for registering caffe2 ops
  RegisterOperators&& op(FunctionSchema schema, Options&& options) && {
    return std::move(*this).op(std::move(options).schema(std::move(schema)));
  }

  template<class FuncType>
  explicit RegisterOperators(const std::string& schemaOrName, FuncType&& func, Options&& options = RegisterOperators::options())
  : RegisterOperators() {
    std::move(*this).op(schemaOrName, std::forward<FuncType>(func), std::move(options));
  }

  /**
   * This API registers an operator based on a kernel function pointer.
   *
   * Given a kernel
   *
   * > namespace { Tensor my_kernel_cpu(Tensor a, Tensor b) {...} }
   *
   * This API looks like:
   *
   * > static auto registry = c10::RegisterOperators()
   * >     .op("my_op", &my_kernel_cpu);
   *
   * If your kernel is small and the overhead of calling it matters,
   * then this API might be the wrong choice since the following API
   * has a slightly lower overhead for calling into the kernel:
   *
   * > static auto registry = c10::RegisterOperators()
   * >     .op("my_op", c10::RegisterOperators::options()
   * >         .kernel<decltype(my_kernel_cpu), &my_kernel_cpu>());
   *
   * Or, alternatively, write your kernel as a functor:
   *
   * > namespace {
   * >   class my_kernel_cpu final : public c10::OperatorKernel {
   * >   public:
   * >     Tensor operator()(Tensor a, Tensor b) {...}
   * >   };
   * > }
   * >
   * > static auto registry = c10::RegisterOperators()
   * >     .op("my_op", c10::RegisterOperators::options()
   * >         .kernel<my_kernel_cpu>());
   */
   template<class FuncType>
   // enable_if: only enable it if FuncType is actually a function, but not a stack based BoxedKernelFunction.
   std::enable_if_t<guts::is_function_type<FuncType>::value && !std::is_same<FuncType, KernelFunction::BoxedKernelFunction>::value, RegisterOperators&&>
   op(const std::string& schemaOrName, FuncType* func, Options&& options = RegisterOperators::options()) && {
     constexpr bool AllowLegacyTypes = true;
     return std::move(*this).op(std::move(options).schema(schemaOrName).kernel(
       c10::nullopt,
       KernelFunction::makeFromUnboxedRuntimeFunction<AllowLegacyTypes>(func),
       // TODO Do schema inference without relying on WrapFunctionIntoRuntimeFunctor
       detail::inferFunctionSchemaFromFunctor<impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<FuncType>>>()
     ));
   }

   /**
    * This API registers an operator based on a kernel lambda.
    *
    * This API looks like:
    *
    * > static auto registry = c10::RegisterOperators()
    * >     .op("my_op", [] (Tensor a, Tensor b) {...});
    *
    * This is equivalent to:
    *
    * > static auto registry = c10::RegisterOperators()
    * >     .op("my_op", c10::RegisterOperators::options()
    * >         .catchAllKernel([] (Tensor a, Tensor b) {...}));
    *
    */
    template<class Lambda>
    // enable_if: only enable it if Lambda is actually a stateless lambda
    std::enable_if_t<guts::is_functor<Lambda>::value && guts::is_stateless_lambda<std::decay_t<Lambda>>::value, RegisterOperators&&>
    op(const std::string& schemaOrName, Lambda&& lambda, Options&& options = RegisterOperators::options()) && {
      static_assert(!std::is_base_of<OperatorKernel, Lambda>::value, "c10::OperatorKernel is part of the new kernel registration API and shouldn't be used together with the deprecated registration API. Please use the new RegisterOperators::options().kernel() based API instead.");

      constexpr bool AllowLegacyTypes = true;
      return std::move(*this).op(std::move(options).schema(schemaOrName).kernel(
        c10::nullopt,
        KernelFunction::makeFromUnboxedLambda<AllowLegacyTypes>(std::forward<Lambda>(lambda)),
        // TODO Do schema inference without relying on WrapFunctionIntoRuntimeFunctor
        detail::inferFunctionSchemaFromFunctor<impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<Lambda>>>()
      ));
    }

    template<class Lambda>
    C10_DEPRECATED_MESSAGE("Registering operator kernels with stateful lambdas (i.e. lambdas with a capture) has non-obvious behavior. This is deprecated. Please use a lambda without a capture or a functor class instead.")
    // enable_if: only enable it if Lambda is actually a functor but not a stateless lambda
    std::enable_if_t<guts::is_functor<Lambda>::value && !guts::is_stateless_lambda<std::decay_t<Lambda>>::value, RegisterOperators&&>
    op(const std::string& schemaOrName, Lambda&& lambda, Options&& options = RegisterOperators::options()) && {
      static_assert(!std::is_base_of<OperatorKernel, Lambda>::value, "c10::OperatorKernel is part of the new kernel registration API and shouldn't be used together with the deprecated registration API. Please use the new RegisterOperators::options().kernel() based API instead.");

      constexpr bool AllowLegacyTypes = true;
      return std::move(*this).op(std::move(options).schema(schemaOrName).kernel(
        c10::nullopt,
        KernelFunction::makeFromUnboxedLambda<AllowLegacyTypes>(std::forward<Lambda>(lambda)),
        // TODO Do schema inference without relying on WrapFunctionIntoRuntimeFunctor
        detail::inferFunctionSchemaFromFunctor<impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<Lambda>>>()
      ));
    }

private:
  void checkSchemaAndRegisterOp_(Options&& config);

  static c10::FunctionSchema inferSchemaFromKernels_(const OperatorName& opNameStr, const Options& options);
  void checkNoDuplicateKernels_(const Options& options);
  void registerOp_(Options&& options);

  std::vector<RegistrationHandleRAII> registrars_;
};

// --------------------------------------------------------------------------
//
// New style API
//
// --------------------------------------------------------------------------
//
// The basic concept behind the new style API is to be as similar to pybind11's
// API as possible.
//
// A quick tour of a few usage examples:
//
//  // Define a library whose operators live in the namespace 'aten'.
//  // You must define all of the operators for this library in
//  // this namespace.
//  TORCH_LIBRARY(aten, m) {
//    // Define a schema for an operator, but provide no implementation
//    m.def("mul(Tensor self, Tensor other) -> Tensor");
//
//    // Define a operator with exactly one implementation for all backends.
//    m.def("add(Tensor self, Tensor other) -> Tensor", &add_impl);
//
//    // Provide an implementation for a defined operator (you can
//    // provide multiple; one per backend).  We'll take care of calling
//    // the correct implementation depending on if we get a CPU
//    // tensor or a CUDA tensor
//    m.impl("mul", torch::kCPU, &mul_cpu_impl);
//    m.impl("mul", torch::kCUDA, &mul_cuda_impl);
//  }
//
//  // Define implementations for operators for a non-standard backend,
//  // e.g., XLA (valid values are entries of DispatchKey).  These
//  // operator names are not namespaced; you can define implementations
//  // for any namespace.
//  TORCH_LIBRARY_IMPL(aten, XLA, m) {
//    m.impl("mul", &mul_xla_impl);
//  }


// Represents a C++ function that implements an operator.  Most users won't
// interact directly with this class, except via error messages: the
// constructors this function define the set of permissible "function"-like
// things you can bind via the interface.
//
// This class erases the type of the passed in function, but durably records
// the type via an inferred schema for the function.
//
// TODO: This is morally the same thing as KernelRegistrationConfig, but it's
// opaque to the user.
class CAFFE2_API CppFunction final {
public:
  // This overload accepts function pointers, e.g., CppFunction(&add_impl)
  template <typename Func>
  explicit CppFunction(Func* f, std::enable_if_t<guts::is_function_type<Func>::value, std::nullptr_t> = nullptr)
    : func_(c10::KernelFunction::makeFromUnboxedRuntimeFunction(f))
    // TODO: Don't go through WrapRuntimeKernelFunctor
    , schema_(detail::inferFunctionSchemaFromFunctor<impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<Func>>>())
    , debug_()
    {}

  // This overload accepts lambdas, e.g., CppFunction([](const Tensor& self) { ... })
  template <typename Lambda>
  explicit CppFunction(Lambda&& f, std::enable_if_t<guts::is_functor<std::decay_t<Lambda>>::value, std::nullptr_t> = nullptr)
    : func_(c10::KernelFunction::makeFromUnboxedLambda(std::forward<Lambda>(f)))
    // TODO: Don't go through WrapRuntimeKernelFunctor
    , schema_(detail::inferFunctionSchemaFromFunctor<impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<Lambda>>>())
    , debug_()
    {}

  // This static factory lets you create CppFunctions that (1) don't have boxing
  // wrappers (because we don't support it yet) and (2) don't have schema
  // inference (because some ops don't support it).
  //
  // TODO: Eliminate the necessity for this function entirely.
  template <typename Func>
  static CppFunction makeUnboxedOnly(Func* f) {
    return CppFunction(
      c10::KernelFunction::makeFromUnboxedOnlyRuntimeFunction(f),
      /* schema */ nullptr
    );
  }

  // TODO: more user friendly API
  static CppFunction makeFallthrough() {
    return CppFunction(
      c10::KernelFunction::makeFallthrough(),
      /* schema */ nullptr
    );
  }

  // TODO: more user friendly API
  template<KernelFunction::BoxedKernelFunction* func>
  static CppFunction makeFromBoxedFunction() {
    return CppFunction(
      c10::KernelFunction::makeFromBoxedFunction<func>(),
      /* schema */ nullptr
    );
  }

  CppFunction&& debug(std::string d) && {
    debug_ = std::move(d);
    return std::move(*this);
  }

private:
  c10::optional<c10::DispatchKey> dispatch_key_;
  c10::KernelFunction func_;
  std::unique_ptr<c10::FunctionSchema> schema_;
  std::string debug_;

  // The "setter" for dispatch_key_
  template <typename Func>
  friend CppFunction dispatch(c10::DispatchKey, Func&&);

  // The only class which actually pulls out values from CppFunction (does so
  // destructively, felt too lazy to write accessors that I don't even
  // want users to use)
  friend class Library;

  CppFunction(KernelFunction func, std::unique_ptr<c10::FunctionSchema> schema);
};

// Create a CppFunction which is associated with a specific dispatch key.
// CppFunctions that are tagged with a DispatchKey don't get invoked /unless/
// the dispatcher determines that the DispatchKey is the best choice for
// a function
template <typename Func>
inline CppFunction dispatch(c10::DispatchKey k, Func&& raw_f) {
  CppFunction f(std::forward<Func>(raw_f));
  if (k == c10::DispatchKey::CatchAll) {
    f.dispatch_key_ = c10::nullopt;
  } else {
    f.dispatch_key_ = k;
  }
  return f;
}

// Convenience overload of dispatch which accepts DeviceType
template <typename Func>
inline CppFunction dispatch(DeviceType type, Func&& raw_f) {
  auto deviceTypeToDispatchKey = [](DeviceType t){
    switch (t) {
      // This list is synchronized with the k-constants in c10/core/DeviceType.h
      case DeviceType::CPU:
        return c10::DispatchKey::CPU;
      case DeviceType::CUDA:
        return c10::DispatchKey::CUDA;
      case DeviceType::XLA:
        return c10::DispatchKey::XLA;
      case DeviceType::HIP:
        return c10::DispatchKey::HIP;
      case DeviceType::MSNPU:
        return c10::DispatchKey::MSNPU;
      default:
        TORCH_CHECK(false,
          "Device type ", t, " cannot be overloaded at dispatch time, "
          "please file a bug report explaining what you were trying to do.");
    }
  };
  return dispatch(deviceTypeToDispatchKey(type), std::forward<Func>(raw_f));
}

inline FunctionSchema schema(const char* str, AliasAnalysisKind k) {
  FunctionSchema s = torch::jit::parseSchema(str);
  s.setAliasAnalysis(k);
  return s;
}
inline FunctionSchema schema(const char* s) {
  return schema(s, AliasAnalysisKind::FROM_SCHEMA);
}
inline FunctionSchema&& schema(FunctionSchema&& s) { return std::move(s); }

namespace detail {

  inline c10::either<OperatorName, FunctionSchema> constructSchemaOrName(FunctionSchema&& s) {
    return c10::make_right<OperatorName, FunctionSchema>(std::move(s));
  }
  inline c10::either<OperatorName, FunctionSchema> constructSchemaOrName(OperatorName&& n) {
    return c10::make_left<OperatorName, FunctionSchema>(std::move(n));
  }
  inline c10::either<OperatorName, FunctionSchema> constructSchemaOrName(const char* str) {
    auto s = torch::jit::parseSchemaOrName(str);
    if (s.is_right()) {
      s.right().setAliasAnalysis(AliasAnalysisKind::FROM_SCHEMA);
    }
    return s;
  }

}

namespace detail {
  class TorchLibraryInit;
}

// This is the "handle" by which functions defined in TORCH_LIBRARY
// and TORCH_LIBRARY_IMPL can define operators and override implementations
// at certain backends.
//
// Conventionally, you get access to it using those two macros:
//
// TORCH_LIBRARY(torchvision, m) {
//    // m is a c10::Library
//    m.def("roi_align", ...);
//    ...
// }
//
// TORCH_LIBRARY_IMPL(aten, XLA, m) {
//    // m is a c10::Library
//    m.impl("add", ...);
//    ...
// }
//
// In some cases, you need to define something that applies to all namespaces,
// not just one namespace (usually a fallback).  In that case, use the reserved
// namespace _, e.g.,
//
// TORCH_LIBRARY_IMPL(_, XLA, m) {
//    m.fallback(xla_fallback);
// }
//
class CAFFE2_API Library final {
public:
  // Which type of macro produced this Library
  enum Kind {
    DEF, // from TORCH_LIBRARY (no qualifier)
    IMPL,
    FRAGMENT,
  };

  // Use TORCH_LIBRARY/TORCH_LIBRARY_IMPL instead of these constructors directly
  Library(Kind kind, std::string ns, c10::optional<DispatchKey> k, const char* file, uint32_t line);

  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  Library(Library&&) = default;
  Library& operator=(Library&&) = default;

  // Some notes about the API design here.  We had the following constraints:
  //
  //  - We need to support multiple "types" of arguments for schema and
  //    functions (e.g., unnamed lambda types, regular functions, const char*,
  //    fully instantiated schemas)
  //  - We don't want to write exponentially many overloads
  //  - We don't want to rely on implicit conversion to a common type,
  //    because the C++ compiler will only be willing to do a single
  //    implicit conversion (reducing the set of valid types which you
  //    can invoke with); also error messages are worse when an implicit
  //    conversion is not selected (as the compiler will not explain
  //    why it didn't select an implicit conversion; this is different
  //    from overloads where it will explain each candidate overload and
  //    why it didn't apply)
  //
  // To solve all of these constraints at the same time, we use a trick taken
  // from the pybind11 library: template over the argument in the user visible
  // API, and inside of the templated function explicitly call an overloaded
  // function to resolve the argument to a real type.  You get the good error
  // messages from overloads, but at the same time you only need to write the
  // overload for any given argument type once.

  // Declare an operator with a schema, but don't provide any implementations
  // for it.  You're expected to then provide implementations using the
  // impl() method.
  template <typename Schema>
  Library& def(Schema&& raw_schema) & {
    FunctionSchema s = schema(std::forward<Schema>(raw_schema));
    return _def(std::move(s));
  }

  // Convenience method to define an operator for a schema and then register
  // an implementation for it.  def(n, f) is almost equivalent to def(n).impl(f),
  // except that if n is not a schema, then the schema is inferred from the
  // static type of f.
  template <typename NameOrSchema, typename Func>
  Library& def(NameOrSchema&& raw_name_or_schema, Func&& raw_f) & {
    CppFunction f(std::forward<Func>(raw_f));
    auto name_or_schema = detail::constructSchemaOrName(std::forward<NameOrSchema>(raw_name_or_schema));
    return _def(std::move(name_or_schema), std::move(f));
  }

  // Register an implementation for an operator.  You may register multiple
  // implementations for a single operator at different dispatch keys
  // (see torch::dispatch).  Implementations must have a corresponding
  // declaration (from def), otherwise they are invalid.
  template <typename Func>
  Library& impl(const char* name, Func&& raw_f) & {
    CppFunction f(std::forward<Func>(raw_f));
    return _impl(name, std::move(f));
  }

  // Convenience overload for directly specifying the dispatch key.  Dispatch
  // can validly be either DeviceType or DispatchKey; check torch::dispatch for
  // the canonical list of accepted overloads.
  template <typename Dispatch, typename Func>
  Library& impl(const char* name, Dispatch&& key, Func&& raw_f) & {
    return impl(name, dispatch(std::forward<Dispatch>(key), std::forward<Func>(raw_f)));
  }

  // Convenience overload for unboxed only kernels.  These are quite common
  // but will be eventually eliminated; this function makes it easy to grep for
  // them.
  //
  // TODO: Remove this overload once the makeUnboxedOnly incidence rate
  // goes way down
  template <typename Func>
  Library& impl_UNBOXED(const char* name, Func* raw_f) & {
    return impl(name, CppFunction::makeUnboxedOnly(raw_f));
  }

  // Register a fallback implementation for all operators which will be used
  // if there is not a specific implementation for an operator available.
  // Providing a DispatchKey is MANDATORY for fallback at the moment; e.g.,
  // only call this from TORCH_LIBRARY_IMPL
  template <typename Func>
  Library& fallback(Func&& raw_f) & {
    CppFunction f((std::forward<Func>(raw_f)));
    return _fallback(std::move(f));
  }

private:
  Kind kind_;
  c10::optional<std::string> ns_;
  c10::optional<DispatchKey> dispatch_key_;
  const char* file_;
  uint32_t line_;

  std::vector<RegistrationHandleRAII> registrars_;

  friend detail::TorchLibraryInit;

  // Non-user visible actual implementations of functions.  These aren't
  // public because we only implement & qualifier and not && qualifier
  Library& _def(FunctionSchema&& schema, OperatorName* out_name = nullptr) &;
  Library& _def(c10::either<OperatorName, FunctionSchema>&&, CppFunction&& f) &;
  Library& _impl(const char* name, CppFunction&& f) &;
  Library& _fallback(CppFunction&& f) &;
};

namespace detail {

class TorchLibraryInit final {
private:
  using InitFn = void(Library&);
  Library lib_;
public:
  TorchLibraryInit(Library::Kind kind, InitFn* fn, const char* ns, c10::optional<DispatchKey> k, const char* file, uint32_t line)
    : lib_(kind, ns, k, file, line) {
    fn(lib_);
  }
};

} // namespace detail

} // namespace c10

// NB: The EXACT NAMING of the initializer functions (e.g.,
// TORCH_LIBRARY_init_aten) matters for the code analyzer;
// see the regexes at tools/code_analyzer/run_analyzer.sh

#define TORCH_LIBRARY(ns, m) \
  static void TORCH_LIBRARY_init_ ## ns (c10::Library&); \
  static c10::detail::TorchLibraryInit TORCH_LIBRARY_static_init_ ## ns ( \
    c10::Library::DEF, \
    &TORCH_LIBRARY_init_ ## ns, \
    #ns, c10::nullopt, __FILE__, __LINE__ \
  ); \
  void TORCH_LIBRARY_init_ ## ns (c10::Library& m)

// This macro is a version of TORCH_LIBRARY that doesn't enforce that there
// is only one library (it is a "fragment").  This should ONLY be used
// with PerOpRegistration (as its name suggests).
#define TORCH_LIBRARY_FRAGMENT_THIS_API_IS_FOR_PER_OP_REGISTRATION_ONLY(ns, m) \
  static void TORCH_LIBRARY_FRAGMENT_init_ ## ns ## _ ## k (c10::Library&); \
  static c10::detail::TorchLibraryInit TORCH_LIBRARY_FRAGMENT_static_init_ ## ns ## _ ## k ( \
    c10::Library::FRAGMENT, \
    &TORCH_LIBRARY_FRAGMENT_init_ ## ns ## _ ## k, \
    #ns, c10::nullopt, __FILE__, __LINE__ \
  ); \
  void TORCH_LIBRARY_FRAGMENT_init_ ## ns ## _ ## k (c10::Library& m)

#define TORCH_LIBRARY_IMPL(ns, k, m) \
  static void TORCH_LIBRARY_IMPL_init_ ## ns ## _ ## k (c10::Library&); \
  static c10::detail::TorchLibraryInit TORCH_LIBRARY_IMPL_static_init_ ## ns ## _ ## k ( \
    c10::Library::IMPL, \
    & TORCH_LIBRARY_IMPL_init_ ## ns ## _ ## k, \
    #ns, c10::make_optional(c10::DispatchKey::k), __FILE__, __LINE__ \
  ); \
  void TORCH_LIBRARY_IMPL_init_ ## ns ## _ ## k (c10::Library& m)

// These are variants of the macros above which are to be used for testing (they
// don't setup the static initializer, so you can control the visibility of
// the allocated library yourself).
//
// DO NOT use these in production code, they are NOT understood by the
// code analyzer and will be incorrectly analyzed in those situations.
#define MAKE_TORCH_LIBRARY(ns) Library(Library::DEF, #ns, c10::nullopt, __FILE__, __LINE__)
#define MAKE_TORCH_LIBRARY_IMPL(ns, k) Library(Library::IMPL, #ns, c10::make_optional(c10::DispatchKey::k), __FILE__, __LINE__)

namespace torch {
  // Old-style API
  using RegisterOperators = c10::RegisterOperators;

  // New-style API
  using c10::dispatch;
  using c10::schema;
}
