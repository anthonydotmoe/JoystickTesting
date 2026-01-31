#pragma once

#include <Windows.h>
#include <oleauto.h>

class ScopedBstr
{
public:
    explicit ScopedBstr(const wchar_t* value)
        : value_(SysAllocString(value))
    {
    }

    ~ScopedBstr()
    {
        if (value_)
            SysFreeString(value_);
    }

    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    [[nodiscard]] BSTR get() const { return value_; }
    [[nodiscard]] bool valid() const { return value_ != nullptr; }

private:
    BSTR value_ = nullptr;
};

class ScopedVariant
{
public:
    ScopedVariant()
    {
        VariantInit(&value_);
    }

    ~ScopedVariant()
    {
        VariantClear(&value_);
    }

    ScopedVariant(const ScopedVariant&) = delete;
    ScopedVariant& operator=(const ScopedVariant&) = delete;

    [[nodiscard]] VARIANT* get() { return &value_; }
    [[nodiscard]] const VARIANT& value() const { return value_; }

private:
    VARIANT value_ = {};
};

class ScopedComInit
{
public:
    ScopedComInit()
        : hr_(CoInitialize(nullptr)),
          shouldUninit_(SUCCEEDED(hr_))
    {
    }

    ~ScopedComInit()
    {
        if (shouldUninit_)
            CoUninitialize();
    }

    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit& operator=(const ScopedComInit&) = delete;

    [[nodiscard]] HRESULT hr() const { return hr_; }
    [[nodiscard]] bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_ = E_FAIL;
    bool shouldUninit_ = false;
};
