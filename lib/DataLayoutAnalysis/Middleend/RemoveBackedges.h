#pragma once

//
// Copyright (c) rev.ng Srls. See LICENSE.md for details.
//

namespace dla {

class LayoutTypeSystem;

extern bool removeInstanceBackedgesFromInheritanceLoops(LayoutTypeSystem &TS);

} // end namespace dla