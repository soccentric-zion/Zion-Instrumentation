// ============================================================================
//  Arches Instrumentation
//
//  Copyright (C) 2026 Soccentric. All rights reserved.
//  Author: Sandesh Ghimire (sandesh@soccentric)
//
//  This file is part of a proprietary product of Soccentric.
//
//  NO WARRANTY:
//  This software is provided "AS IS", without warranty of any kind, either
//  express or implied, including but not limited to the warranties of
//  merchantability, fitness for a particular purpose, and non-infringement.
//  In no event shall Soccentric be liable for any claim, damages, or other
//  liability arising from the use of this software.
//
//  This software may not be used, copied, modified, distributed, or
//  disclosed, in whole or in part, without the express prior written
//  consent of Soccentric.
// ============================================================================
/**
 * @file catalog/src/alarmgen_main.cpp
 * @brief builds the shared alarm ID header from catalog/alarms.yaml
 *
 * @details
 * Thin IO shell for the pure code-generation domain
 * (instr/catalog/codegen.h). At build time this tool is executed by the
 * CMake build system with two positional arguments — the path of the YAML
 * catalog and the path of the generated header — and does the following:
 *
 * 1. Reads the entire catalog file into memory.
 * 2. Calls instr::catalog::parse_catalog to parse and validate the catalog
 *    (duplicates, unknown severities, and malformed ids are rejected with a
 *    diagnosable message).
 * 3. Calls instr::catalog::render_header to render the shared C++ header
 *    (INSTR_ALARM_* constants + entry table), stamping the repository-wide
 *    INSTR_VERSION.
 * 4. Writes the rendered text to the output path.
 *
 * Separation of concerns: all domain logic lives in the pure
 * instr::catalog namespace unit, which the unit tests exercise
 * directly (no filesystem access whatsoever); this translation unit only
 * handles file IO, argument checking, and exit codes.
 *
 * @par Exit codes
 * | Code | Meaning |
 * |------|---------|
 * | 0    | The header was generated successfully. |
 * | 1    | Domain failure: the catalog was rejected, or header rendering failed. |
 * | 2    | Usage/IO failure: incorrect number of arguments, or input/output file could not be opened. |
 *
 * @author Sandesh Ghimire (sandesh@soccentric)
 * @date 2026
 * @copyright (C) 2026 Soccentric. All rights reserved. Proprietary.
 */

#include <fstream>
#include <iostream>

#include "instr/catalog/codegen.h"
#include "instr/common/version_gen.h"

/**
 * @brief alarmgen tool entry: parse the catalog YAML and emit the C++ header.
 *
 * @param[in] argc Number of program arguments.
 * @param[in] argv Program arguments; expects two positionals:
 *           argv[1] = path to the input catalog YAML (must exist and be readable),
 *           argv[2] = path to the output C++ header (must be writable).
 * @return 0 on success; 1 on catalog/render failure; 2 on usage/IO failure
 *         (see the file description for the exit-code contract).
 *
 * All failure modes print a single-line message to stderr naming the
 * offending file or domain error, so failures surface with a clear reason in
 * CMake build logs.
 */
int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: alarmgen <catalog.yaml> <output-header.h>\n";
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in)
    {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto cat = instr::catalog::parse_catalog(text);
    if (!cat.ok())
    {
        std::cerr << "catalog rejected: " << cat.error.str() << "\n";
        return 1;
    }
    auto hdr = instr::catalog::render_header(*cat, INSTR_VERSION);
    if (!hdr.ok())
    {
        std::cerr << "header render failed: " << hdr.error.str() << "\n";
        return 1;
    }
    std::ofstream out(argv[2]);
    if (!out)
    {
        std::cerr << "cannot write " << argv[2] << "\n";
        return 2;
    }
    out << *hdr;
    return 0;
}
