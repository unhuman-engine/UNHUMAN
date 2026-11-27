#pragma once

#include "UHE/Core/Core.h"

namespace UHE::RHI
{
class RHIDevice;
}

namespace UHE
{

/// @brief How an RHI self-test run went.
struct RHISelfTestResult
{
    u32 Checks = 0;
    u32 Failures = 0;
};

/**
 * @brief Exercises the RHI thin core (issue #30) on the live device, through the public RHI only.
 *
 * Barriers, buffer and texture copies, blits, a texture view and an imported
 * texture, timestamps, a compute dispatch through descriptor set 1 with
 * indirect arguments (and push descriptors when the device has them), a
 * secondary command list, the compute queue, a rendering scope's clear and a
 * multisample resolve. Every result is read back and compared; each check logs
 * one `[RHI-SELFTEST]` line.
 *
 * Run by any app when UHE_RHI_SELFTEST is set (Application); with
 * UHE_RHI_SELFTEST=exit the app exits after it, with status 1 on a failure.
 * CI runs it on lavapipe under UHE_VALIDATION_FATAL, so a validation error in
 * any of it fails the run too.
 */
UHE_API RHISelfTestResult RunRHISelfTest(RHI::RHIDevice& device);

} // namespace UHE
