#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <format>
#include <array>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif

void get_regs();
void get_regs_end();
void set_regs();
void set_regs_end();

#ifdef __cplusplus
}
#endif

struct RegContext {
    union FPReg {
        __int128_t q;
        struct {
            double d1;
            double d2;
        } d;
        struct {
            float f1;
            float f2;
            float f3;
            float f4;
        } f;
    };
    static_assert(sizeof(FPReg) == 16, "FPReg size mismatch");

    struct SRegView {
        FPReg data[32];
        float&       operator[](size_t n)       { return data[n].f.f1; }
        const float& operator[](size_t n) const { return data[n].f.f1; }
    };

    struct DRegView {
        FPReg data[32];
        double&       operator[](size_t n)       { return data[n].d.d1; }
        const double& operator[](size_t n) const { return data[n].d.d1; }
    };

    union {
        SRegView s;  // s[n] == q[n].f.f1 == Arm64 sN
        DRegView d;  // d[n] == q[n].d.d1 == Arm64 dN
        FPReg q[32];
        struct {
        FPReg q0, q1, q2, q3, q4, q5, q6, q7;
        // [!!! READ ME !!!]
        // for Arm64, can't access q8 - q31, unless you enable full floating-point register pack
        FPReg q8, q9, q10, q11, q12, q13, q14, q15, q16, q17, q18, q19, q20, q21, q22, q23, q24, q25, q26, q27, q28, q29,
            q30, q31;
        } regs;
    } floating;

    union {
        uint64_t x[29];
        struct {
        uint64_t x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15, x16, x17, x18, x19, x20, x21, x22,
            x23, x24, x25, x26, x27, x28;
        } regs;
    } general;

    uint64_t fp;
    uint64_t lr;
    uint64_t sp;

    uint64_t nzcv;

    uint64_t _pad;

    std::string ToString() const
    {
        std::string result;
        for (int i = 0; i < 29; i++) {
            result += std::format("x{}: {:#016x}\n", i, general.x[i]);
        }
        result += std::format("fp: {:#016x}\n", fp);
        result += std::format("lr: {:#016x}\n", lr);
        result += std::format("sp: {:#016x}\n", sp);
        result += std::format("nzcv: {:#016x}\n", nzcv);
        return result;
    }
};
static_assert(sizeof(RegContext) == 0x310, "Wrong size on RegContext");
static_assert(offsetof(RegContext, floating) == 0x0, "Wrong offset on floating");
static_assert(offsetof(RegContext, general) == 0x200, "Wrong offset on general");
static_assert(offsetof(RegContext, fp) == 0x2E8, "Wrong offset on fp");
static_assert(offsetof(RegContext, lr) == 0x2F0, "Wrong offset on lr");
static_assert(offsetof(RegContext, sp) == 0x2F8, "Wrong offset on sp");
static_assert(offsetof(RegContext, nzcv) == 0x300, "Wrong offset on nzcv");

class IPointerHook
{
public:
    IPointerHook();
    virtual ~IPointerHook();

    virtual std::string GetName() const = 0;

    virtual uintptr_t FakeFunction(RegContext *ctx) = 0;

    virtual void Initialize();

    virtual void InstallHook();
    
    virtual void RestoreHook();

    virtual void DestroyHook();

    uintptr_t GetElfBase() const { return GetElfBaseImpl(); }

    uintptr_t GetOrigPtrAddr() const { return orig_ptr_addr_; }

    uintptr_t GetOrigFuncAddr() const { return orig_func_addr_; }

    uintptr_t GetFakeFuncAddr() const { return fake_func_addr_; }

    template <typename Ret, typename... Args>
    Ret CallOrigFunction(Args... args);

    /**
     * @warning Unsafe: only use when you are sure about the calling convention and arguments
     */
    template <typename Ret>
    Ret CallOrigWithContext(RegContext *ctx);

protected:
    virtual uintptr_t GetElfBaseImpl() const = 0;
    virtual uintptr_t GetPtrAddrImpl() const = 0;
    virtual uintptr_t GetFuncAddrImpl() const = 0;

    virtual void *x_mmap(size_t size);
    virtual bool x_munmap(void *addr, size_t size);

private:
    virtual bool PrepareTrampoline();

    static uintptr_t Dispatcher(RegContext *ctx, uint32_t index);

private:
    bool initialized_;
    bool prepared_;
    bool installed_;

    uint32_t index_;

    uint64_t orig_ptr_addr_;
    uint64_t orig_func_addr_;
    uint64_t fake_func_addr_;

private:
    static constexpr size_t MAX_HOOKS = 512;
    static std::array<std::atomic<IPointerHook*>, MAX_HOOKS> g_Hacks_;
    static std::atomic<uint32_t> g_Index_;
};

template <typename Ret, typename... Args>
Ret IPointerHook::CallOrigFunction(Args... args)
{
    using orig_func_t = Ret (*)(Args...);
    orig_func_t f = (orig_func_t)GetOrigFuncAddr();
    return f(args...);
}

template <typename Ret>
Ret IPointerHook::CallOrigWithContext(RegContext *ctx)
{
    Ret ret;
    uintptr_t func = GetOrigFuncAddr();
    void *regs = (void *)&ctx->general;
    __asm__ volatile (
        "ldp x0, x1, [%[ctx], #0x00]\n"
        "ldp x2, x3, [%[ctx], #0x10]\n"
        "ldp x4, x5, [%[ctx], #0x20]\n"
        "ldp x6, x7, [%[ctx], #0x30]\n"
        "blr %[func]\n"
        "mov %[ret], x0\n"
        : [ret] "=r"(ret)
        : [ctx] "r"(regs), [func] "r"(func)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", 
          "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
          "cc", "memory"
    );
    return ret;
}
