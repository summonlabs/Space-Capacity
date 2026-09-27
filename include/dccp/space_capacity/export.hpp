// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Space Capacity - public declaration macro.

#pragma once

// Space Capacity is built and installed as a static library, so this macro
// expands to nothing on every platform and in every supported configuration.
// It is kept because the public declarations are annotated uniformly, and the
// alternative - annotating them with nothing at all - would make a future
// change of linkage look like a header rewrite rather than a build change.
//
// A shared build is not supported by this release and is not claimed as proved.

#define SC_API
