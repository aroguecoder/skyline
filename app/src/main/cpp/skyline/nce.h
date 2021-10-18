// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#pragma once

#include "common.h"
#include <sys/wait.h>

namespace skyline::nce {
    /**
     * @brief The NCE (Native Code Execution) class is responsible for managing state relevant to the layer between the host and guest
     */
    class NCE {
      private:
        const DeviceState &state;

        static void SvcHandler(u16 svcId, ThreadContext *ctx);

      public:
        /**
         * @brief An exception which causes the throwing thread to exit alongside all threads optionally
         * @note Exiting must not be performed directly as it could leak temporary objects on the stack by not calling their destructors
         */
        struct ExitException : std::exception {
            bool killAllThreads; //!< If to kill all threads or just the throwing thread

            ExitException(bool killAllThreads = true);

            virtual const char* what() const noexcept;
        };

        /**
         * @brief Handles any signals in the NCE threads
         */
        static void SignalHandler(int signal, siginfo *info, ucontext *ctx, void **tls);

        NCE(const DeviceState &state);

        struct PatchData {
            size_t size; //!< Size of the .patch section
            std::vector<size_t> offsets; //!< Offsets in .text of instructions that need to be patched
        };

        static PatchData GetPatchData(const std::vector<u8> &text);

        /**
         * @brief Writes the .patch section and mutates the code accordingly
         * @param patch A pointer to the .patch section which should be exactly patchSize in size and located before the .text section
         */
        static void PatchCode(std::vector<u8> &text, u32 *patch, size_t patchSize, const std::vector<size_t> &offsets);
    };
}
