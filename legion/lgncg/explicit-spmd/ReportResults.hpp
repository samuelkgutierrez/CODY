/**
 * Copyright (c)      2017 Los Alamos National Security, LLC
 *                         All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * LA-CC 10-123
 */

//@HEADER
// ***************************************************
//
// HPCG: High Performance Conjugate Gradient Benchmark
//
// Contact:
// Michael A. Heroux ( maherou@sandia.gov)
// Jack Dongarra     (dongarra@eecs.utk.edu)
// Piotr Luszczek    (luszczek@eecs.utk.edu)
//
// ***************************************************
//@HEADER

/*!
    @file ReportResults.cpp

    HPCG routine
 */

#pragma once

#include "hpcg.hpp"
#include "LegionMatrices.hpp"
#include "YAML_Element.hpp"
#include "YAML_Doc.hpp"
#include "OptimizeProblem.hpp"

#include <fstream>
#include <vector>

/*!
    Creates a YAML file and writes the information about the HPCG run, its
    results, and validity.

    @param[in] geom The description of the problem's geometry.

    @param[in] A    The known system matrix.

    @param[in] numberOfMgLevels Number of levels in multigrid V cycle.

    @param[in] numberOfCgSets Number of CG runs performed.

    @param[in] niters Number of preconditioned CG iterations performed to lower
               the residual below a threshold.

    @param[in] times  Vector of cumulative timings for each of the phases of a
                      preconditioned CG iteration.

    @param[in] testcg_data    The data structure with the results of the
                              CG-correctness test including pass/fail
                              information.

    @param[in] testsymmetry_data The data structure with the results of the CG
                                 symmetry test including pass/fail information.

    @param[in] testnorms_data The data structure with the results of the CG norm
                              test including pass/fail information.

    @param[in] global_failure indicates whether a failure occured during the
                              correctness tests of CG.

    @see YAML_Doc
*/
inline void
ReportResults(
    SparseMatrix &A,
    int numberOfMgLevels,
    int numberOfCgSets,
    int refMaxIters,
    int optMaxIters,
    double times[],
    const TestCGData &testcg_data,
    const TestSymmetryData &testsymmetry_data,
    const TestNormsData &testnorms_data,
    int global_failure,
    bool quickPath,
    Context ctx,
    HighLevelRuntime *lrt
) {
    const auto *const Asclrs = A.sclrs->data();
    const Geometry *Ageom = A.geom->data();
    // Any official benchmark result much run at least this many seconds.
    double minOfficialTime = 1800;

    double t4 = times[4];
    double t4min = 0.0;
    double t4max = 0.0;
    double t4avg = 0.0;
    //
    t4min = allReduce(t4, *A.dcAllRedMinFT, ctx, lrt);
    t4max = allReduce(t4, *A.dcAllRedMaxFT, ctx, lrt);
    t4avg = allReduce(t4, *A.dcAllRedSumFT, ctx, lrt);
    t4avg = t4avg / ((double)Ageom->size);

    // initialize YAML doc

    if (Ageom->rank == 0) { // Only PE 0 needs to compute and report timing results

        // TODO: Put the FLOP count, Memory BW and Memory Usage models into separate functions

        // ======================== FLOP count model =======================================

        double fNumberOfCgSets = numberOfCgSets;
        double fniters = fNumberOfCgSets * (double) optMaxIters;
        double fnrow = Asclrs->totalNumberOfRows;
        double fnnz = Asclrs->totalNumberOfNonzeros;

        // Op counts come from implementation of CG in CG.cpp (include 1 extra for the CG preamble ops)
        double fnops_ddot = (3.0 * fniters + fNumberOfCgSets) * 2.0 * fnrow; // 3 ddots with nrow adds and nrow mults
        double fnops_waxpby = (3.0 * fniters + fNumberOfCgSets) * 2.0 * fnrow; // 3 WAXPBYs with nrow adds and nrow mults
        double fnops_sparsemv = (fniters + fNumberOfCgSets) * 2.0 * fnnz; // 1 SpMV with nnz adds and nnz mults
        // Op counts from the multigrid preconditioners
        double fnops_precond = 0.0;
        const SparseMatrix *Af = &A;
        for (int i = 1; i < numberOfMgLevels; ++i) {
            const auto *const Afsclrs = Af->sclrs->data();
            double fnnz_Af = Afsclrs->totalNumberOfNonzeros;
            double fnumberOfPresmootherSteps = Af->mgData->numberOfPresmootherSteps;
            double fnumberOfPostsmootherSteps = Af->mgData->numberOfPostsmootherSteps;
            fnops_precond += fnumberOfPresmootherSteps * fniters * 4.0 * fnnz_Af; // number of presmoother flops
            fnops_precond += fniters * 2.0 * fnnz_Af; // cost of fine grid residual calculation
            fnops_precond += fnumberOfPostsmootherSteps * fniters * 4.0 * fnnz_Af; // number of postsmoother flops
            Af = Af->Ac; // Go to next coarse level
        }

        fnops_precond += fniters * 4.0 * ((double) Af->sclrs->data()->totalNumberOfNonzeros); // One symmetric GS sweep at the coarsest level
        double fnops = fnops_ddot + fnops_waxpby + fnops_sparsemv + fnops_precond;
        double frefnops = fnops * ((double) refMaxIters) / ((double) optMaxIters);

        // ======================== Memory bandwidth model =======================================

        // Read/Write counts come from implementation of CG in CG.cpp (include 1 extra for the CG preamble ops)
        double fnreads_ddot = (3.0 * fniters + fNumberOfCgSets) * 2.0 * fnrow * sizeof(double); // 3 ddots with 2 nrow reads
        double fnwrites_ddot = (3.0 * fniters + fNumberOfCgSets) * sizeof(double); // 3 ddots with 1 write
        double fnreads_waxpby = (3.0 * fniters + fNumberOfCgSets) * 2.0 * fnrow * sizeof(double); // 3 WAXPBYs with nrow adds and nrow mults
        double fnwrites_waxpby = (3.0 * fniters + fNumberOfCgSets) * fnrow * sizeof(double); // 3 WAXPBYs with nrow adds and nrow mults
        double fnreads_sparsemv = (fniters + fNumberOfCgSets) * (fnnz * (sizeof(double) + sizeof(local_int_t)) + fnrow * sizeof(double)); // 1 SpMV with nnz reads of values, nnz reads indices,
        // plus nrow reads of x
        double fnwrites_sparsemv = (fniters + fNumberOfCgSets) * fnrow * sizeof(double); // 1 SpMV nrow writes
        // Op counts from the multigrid preconditioners
        double fnreads_precond = 0.0;
        double fnwrites_precond = 0.0;
        Af = &A;
        for (int i = 1; i < numberOfMgLevels; ++i) {
            const auto *const Afsclrs = Af->sclrs->data();
            double fnnz_Af = Afsclrs->totalNumberOfNonzeros;
            double fnrow_Af = Afsclrs->totalNumberOfRows;
            double fnumberOfPresmootherSteps = Af->mgData->numberOfPresmootherSteps;
            double fnumberOfPostsmootherSteps = Af->mgData->numberOfPostsmootherSteps;
            fnreads_precond += fnumberOfPresmootherSteps * fniters * (2.0 * fnnz_Af * (sizeof(double) + sizeof(local_int_t)) + fnrow_Af * sizeof(double)); // number of presmoother reads
            fnwrites_precond += fnumberOfPresmootherSteps * fniters * fnrow_Af * sizeof(double); // number of presmoother writes
            fnreads_precond += fniters * (fnnz_Af * (sizeof(double) + sizeof(local_int_t)) + fnrow_Af * sizeof(double)); // Number of reads for fine grid residual calculation
            fnwrites_precond += fniters * fnnz_Af * sizeof(double); // Number of writes for fine grid residual calculation
            fnreads_precond += fnumberOfPostsmootherSteps * fniters * (2.0 * fnnz_Af * (sizeof(double) + sizeof(local_int_t)) + fnrow_Af * sizeof(double)); // number of postsmoother reads
            fnwrites_precond += fnumberOfPostsmootherSteps * fniters * fnnz_Af * sizeof(double); // number of postsmoother writes
            Af = Af->Ac; // Go to next coarse level
        }

        double fnnz_Af = Af->sclrs->data()->totalNumberOfNonzeros;
        double fnrow_Af = Af->sclrs->data()->totalNumberOfRows;
        fnreads_precond += fniters * (2.0 * fnnz_Af * (sizeof(double) + sizeof(local_int_t)) + fnrow_Af * sizeof(double));; // One symmetric GS sweep at the coarsest level
        fnwrites_precond += fniters * fnrow_Af * sizeof(double); // One symmetric GS sweep at the coarsest level
        double fnreads = fnreads_ddot + fnreads_waxpby + fnreads_sparsemv + fnreads_precond;
        double fnwrites = fnwrites_ddot + fnwrites_waxpby + fnwrites_sparsemv + fnwrites_precond;
        double frefnreads = fnreads * ((double) refMaxIters) / ((double) optMaxIters);
        double frefnwrites = fnwrites * ((double) refMaxIters) / ((double) optMaxIters);


        // ======================== Memory usage model =======================================

        // Data in GenerateProblem_ref

        double numberOfNonzerosPerRow = 27.0; // We are approximating a 27-point finite element/volume/difference 3D stencil
        double size = ((double) Ageom->size); // Needed for estimating size of halo

        double fnbytes = ((double) sizeof(Geometry));      // Geometry struct in main.cpp
        fnbytes += ((double) sizeof(double) * fNumberOfCgSets); // testnorms_data in main.cpp

        // Model for GenerateProblem_ref.cpp
        fnbytes += fnrow * sizeof(char);    // array nonzerosInRow
        fnbytes += fnrow * ((double) sizeof(global_int_t *)); // mtxIndG
        fnbytes += fnrow * ((double) sizeof(local_int_t *)); // mtxIndL
        fnbytes += fnrow * ((double) sizeof(double *));   // matrixValues
        fnbytes += fnrow * ((double) sizeof(double *));   // matrixDiagonal
        fnbytes += fnrow * numberOfNonzerosPerRow * ((double) sizeof(local_int_t)); // mtxIndL[1..nrows]
        fnbytes += fnrow * numberOfNonzerosPerRow * ((double) sizeof(double));   // matrixValues[1..nrows]
        fnbytes += fnrow * numberOfNonzerosPerRow * ((double) sizeof(global_int_t)); // mtxIndG[1..nrows]
        fnbytes += fnrow * ((double) 3 * sizeof(double)); // x, b, xexact

        // Model for CGData.hpp
        double fncol = ((global_int_t) Asclrs->localNumberOfColumns) * size; // Estimate of the global number of columns using the value from rank 0
        fnbytes += fnrow * ((double) 2 * sizeof(double)); // r, Ap
        fnbytes += fncol * ((double) 2 * sizeof(double)); // z, p

        std::vector<double> fnbytesPerLevel(numberOfMgLevels); // Count byte usage per level (level 0 is main CG level)
        fnbytesPerLevel[0] = fnbytes;

        // Benchmarker-provided model for OptimizeProblem.cpp
        double fnbytes_OptimizedProblem = OptimizeProblemMemoryUse(A);
        fnbytes += fnbytes_OptimizedProblem;

        Af = A.Ac;
        for (int i = 1; i < numberOfMgLevels; ++i) {
            const auto *const Afsclrs = Af->sclrs->data();
            double fnrow_Af = Afsclrs->totalNumberOfRows;
            double fncol_Af = ((global_int_t) Afsclrs->localNumberOfColumns) * size; // Estimate of the global number of columns using the value from rank 0
            double fnbytes_Af = 0.0;
            // Model for GenerateCoarseProblem.cpp
            fnbytes_Af += fnrow_Af * ((double) sizeof(local_int_t)); // f2cOperator
            fnbytes_Af += fnrow_Af * ((double) sizeof(double)); // rc
            fnbytes_Af += 2.0 * fncol_Af * ((double) sizeof(double)); // xc, Axf are estimated based on the size of these arrays on rank 0
            fnbytes_Af += ((double)(sizeof(Geometry) + sizeof(SparseMatrix) + 3 * sizeof(Array<floatType>) + sizeof(MGData))); // Account for structs geomc, Ac, rc, xc, Axf - (minor)

            // Model for GenerateProblem.cpp (called within GenerateCoarseProblem.cpp)
            fnbytes_Af += fnrow_Af * sizeof(char);    // array nonzerosInRow
            fnbytes_Af += fnrow_Af * ((double) sizeof(global_int_t *)); // mtxIndG
            fnbytes_Af += fnrow_Af * ((double) sizeof(local_int_t *)); // mtxIndL
            fnbytes_Af += fnrow_Af * ((double) sizeof(double *));   // matrixValues
            fnbytes_Af += fnrow_Af * ((double) sizeof(double *));   // matrixDiagonal
            fnbytes_Af += fnrow_Af * numberOfNonzerosPerRow * ((double) sizeof(local_int_t)); // mtxIndL[1..nrows]
            fnbytes_Af += fnrow_Af * numberOfNonzerosPerRow * ((double) sizeof(double));   // matrixValues[1..nrows]
            fnbytes_Af += fnrow_Af * numberOfNonzerosPerRow * ((double) sizeof(global_int_t)); // mtxIndG[1..nrows]

            // Model for SetupHalo.hpp.
            //sendBuffer
            fnbytes_Af += ((double) sizeof(double) * Af->sclrs->data()->totalToBeSent);
            // elementsToSend
            fnbytes_Af += ((double) sizeof(local_int_t) * Af->sclrs->data()->totalToBeSent);
            // neighbors.
            fnbytes_Af += ((double) sizeof(int) * Af->sclrs->data()->numberOfSendNeighbors);
            // receiveLength, sendLength.
            fnbytes_Af += ((double) sizeof(local_int_t) * Af->sclrs->data()->numberOfSendNeighbors);
            fnbytesPerLevel[i] = fnbytes_Af;
            fnbytes += fnbytes_Af; // Running sum
            Af = Af->Ac; // Go to next coarse level
        }

        assert(Af == 0); // Make sure we got to the lowest grid level

        // Count number of bytes used per equation
        double fnbytesPerEquation = fnbytes / fnrow;


        // Instantiate YAML document
        YAML_Doc doc("HPCG-Benchmark", "3.0");
        doc.add("Release date", "November 11, 2015");

        doc.add("Machine Summary", "");
        doc.get("Machine Summary")->add("Distributed Processes", Ageom->size);
        doc.get("Machine Summary")->add("Threads per processes", Ageom->numThreads);

        doc.add("Global Problem Dimensions", "");
        doc.get("Global Problem Dimensions")->add("Global nx", Ageom->npx * Ageom->nx);
        doc.get("Global Problem Dimensions")->add("Global ny", Ageom->npy * Ageom->ny);
        doc.get("Global Problem Dimensions")->add("Global nz", Ageom->npz * Ageom->nz);

        doc.add("Processor Dimensions", "");
        doc.get("Processor Dimensions")->add("npx", Ageom->npx);
        doc.get("Processor Dimensions")->add("npy", Ageom->npy);
        doc.get("Processor Dimensions")->add("npz", Ageom->npz);

        doc.add("Local Domain Dimensions", "");
        doc.get("Local Domain Dimensions")->add("nx", Ageom->nx);
        doc.get("Local Domain Dimensions")->add("ny", Ageom->ny);
        doc.get("Local Domain Dimensions")->add("nz", Ageom->nz);

        doc.add("########## Problem Summary  ##########", "");

        doc.add("Setup Information", "");
        doc.get("Setup Information")->add("Setup Time", times[9]);

        doc.add("Linear System Information", "");
        doc.get("Linear System Information")->add("Number of Equations", Asclrs->totalNumberOfRows);
        doc.get("Linear System Information")->add("Number of Nonzero Terms", Asclrs->totalNumberOfNonzeros);

        doc.add("Multigrid Information", "");
        doc.get("Multigrid Information")->add("Number of coarse grid levels", numberOfMgLevels - 1);
        Af = &A;
        doc.get("Multigrid Information")->add("Coarse Grids", "");
        for (int i = 1; i < numberOfMgLevels; ++i) {
            doc.get("Multigrid Information")->get("Coarse Grids")->add("Grid Level", i);
            doc.get("Multigrid Information")->get("Coarse Grids")->add("Number of Equations", Af->Ac->sclrs->data()->totalNumberOfRows);
            doc.get("Multigrid Information")->get("Coarse Grids")->add("Number of Nonzero Terms", Af->Ac->sclrs->data()->totalNumberOfNonzeros);
            doc.get("Multigrid Information")->get("Coarse Grids")->add("Number of Presmoother Steps", Af->mgData->numberOfPresmootherSteps);
            doc.get("Multigrid Information")->get("Coarse Grids")->add("Number of Postsmoother Steps", Af->mgData->numberOfPostsmootherSteps);
            Af = Af->Ac;
        }

        doc.add("########## Memory Use Summary  ##########", "");

        doc.add("Memory Use Information", "");
        doc.get("Memory Use Information")->add("Total memory used for data (Gbytes)", fnbytes / 1000000000.0);
        doc.get("Memory Use Information")->add("Memory used for OptimizeProblem data (Gbytes)", fnbytes_OptimizedProblem / 1000000000.0);
        doc.get("Memory Use Information")->add("Bytes per equation (Total memory / Number of Equations)", fnbytesPerEquation);

        doc.get("Memory Use Information")->add("Memory used for linear system and CG (Gbytes)", fnbytesPerLevel[0] / 1000000000.0);

        doc.get("Memory Use Information")->add("Coarse Grids", "");
        for (int i = 1; i < numberOfMgLevels; ++i) {
            doc.get("Memory Use Information")->get("Coarse Grids")->add("Grid Level", i);
            doc.get("Memory Use Information")->get("Coarse Grids")->add("Memory used", fnbytesPerLevel[i] / 1000000000.0);
        }

        doc.add("########## V&V Testing Summary  ##########", "");
        doc.add("Spectral Convergence Tests", "");
        if (testcg_data.count_fail == 0) {
            doc.get("Spectral Convergence Tests")->add("Result", "PASSED");
        }
        else {
            doc.get("Spectral Convergence Tests")->add("Result", "FAILED");
        }
        doc.get("Spectral Convergence Tests")->add("Unpreconditioned", "");
        doc.get("Spectral Convergence Tests")->get("Unpreconditioned")->add("Maximum iteration count", testcg_data.niters_max_no_prec);
        doc.get("Spectral Convergence Tests")->get("Unpreconditioned")->add("Expected iteration count", testcg_data.expected_niters_no_prec);
        doc.get("Spectral Convergence Tests")->add("Preconditioned", "");
        doc.get("Spectral Convergence Tests")->get("Preconditioned")->add("Maximum iteration count", testcg_data.niters_max_prec);
        doc.get("Spectral Convergence Tests")->get("Preconditioned")->add("Expected iteration count", testcg_data.expected_niters_prec);

        const char DepartureFromSymmetry[] = "Departure from Symmetry |x'Ay-y'Ax|/(2*||x||*||A||*||y||)/epsilon";
        doc.add(DepartureFromSymmetry, "");
        if (testsymmetry_data.count_fail == 0) {
            doc.get(DepartureFromSymmetry)->add("Result", "PASSED");
        }
        else {
            doc.get(DepartureFromSymmetry)->add("Result", "FAILED");
        }
        doc.get(DepartureFromSymmetry)->add("Departure for SpMV", testsymmetry_data.depsym_spmv);
        doc.get(DepartureFromSymmetry)->add("Departure for MG", testsymmetry_data.depsym_mg);

        doc.add("########## Iterations Summary  ##########", "");
        doc.add("Iteration Count Information", "");
        if (!global_failure) {
            doc.get("Iteration Count Information")->add("Result", "PASSED");
        }
        else {
            doc.get("Iteration Count Information")->add("Result", "FAILED");
        }
        doc.get("Iteration Count Information")->add("Reference CG iterations per set", refMaxIters);
        doc.get("Iteration Count Information")->add("Optimized CG iterations per set", optMaxIters);
        doc.get("Iteration Count Information")->add("Total number of reference iterations", refMaxIters * numberOfCgSets);
        doc.get("Iteration Count Information")->add("Total number of optimized iterations", optMaxIters * numberOfCgSets);

        doc.add("########## Reproducibility Summary  ##########", "");
        doc.add("Reproducibility Information", "");
        if (testnorms_data.pass) {
            doc.get("Reproducibility Information")->add("Result", "PASSED");
        }
        else {
            doc.get("Reproducibility Information")->add("Result", "FAILED");
        }
        doc.get("Reproducibility Information")->add("Scaled residual mean", testnorms_data.mean);
        doc.get("Reproducibility Information")->add("Scaled residual variance", testnorms_data.variance);

        doc.add("########## Performance Summary (times in sec) ##########", "");

        doc.add("Benchmark Time Summary", "");
        doc.get("Benchmark Time Summary")->add("Optimization phase", times[7]);
        doc.get("Benchmark Time Summary")->add("DDOT", times[1]);
        doc.get("Benchmark Time Summary")->add("WAXPBY", times[2]);
        doc.get("Benchmark Time Summary")->add("SpMV", times[3]);
        doc.get("Benchmark Time Summary")->add("MG", times[5]);
        doc.get("Benchmark Time Summary")->add("Total", times[0]);

        doc.add("Floating Point Operations Summary", "");
        doc.get("Floating Point Operations Summary")->add("Raw DDOT", fnops_ddot);
        doc.get("Floating Point Operations Summary")->add("Raw WAXPBY", fnops_waxpby);
        doc.get("Floating Point Operations Summary")->add("Raw SpMV", fnops_sparsemv);
        doc.get("Floating Point Operations Summary")->add("Raw MG", fnops_precond);
        doc.get("Floating Point Operations Summary")->add("Total", fnops);
        doc.get("Floating Point Operations Summary")->add("Total with convergence overhead", frefnops);

        doc.add("GB/s Summary", "");
        doc.get("GB/s Summary")->add("Raw Read B/W", fnreads / times[0] / 1.0E9);
        doc.get("GB/s Summary")->add("Raw Write B/W", fnwrites / times[0] / 1.0E9);
        doc.get("GB/s Summary")->add("Raw Total B/W", (fnreads + fnwrites) / (times[0]) / 1.0E9);
        doc.get("GB/s Summary")->add("Total with convergence and optimization phase overhead", (frefnreads + frefnwrites) / (times[0] + fNumberOfCgSets * (times[7] / 10.0 + times[9] / 10.0)) / 1.0E9);


        doc.add("GFLOP/s Summary", "");
        doc.get("GFLOP/s Summary")->add("Raw DDOT", fnops_ddot / times[1] / 1.0E9);
        doc.get("GFLOP/s Summary")->add("Raw WAXPBY", fnops_waxpby / times[2] / 1.0E9);
        doc.get("GFLOP/s Summary")->add("Raw SpMV", fnops_sparsemv / (times[3]) / 1.0E9);
        doc.get("GFLOP/s Summary")->add("Raw MG", fnops_precond / (times[5]) / 1.0E9);
        doc.get("GFLOP/s Summary")->add("Raw Total", fnops / times[0] / 1.0E9);
        doc.get("GFLOP/s Summary")->add("Total with convergence overhead", frefnops / times[0] / 1.0E9);
        // This final GFLOP/s rating includes the overhead of problem setup and optimizing the data structures vs ten sets of 50 iterations of CG
        double totalGflops = frefnops / (times[0] + fNumberOfCgSets * (times[7] / 10.0 + times[9] / 10.0)) / 1.0E9;
        double totalGflops24 = frefnops / (times[0] + fNumberOfCgSets * times[7] / 10.0) / 1.0E9;
        doc.get("GFLOP/s Summary")->add("Total with convergence and optimization phase overhead", totalGflops);

        doc.add("User Optimization Overheads", "");
        doc.get("User Optimization Overheads")->add("Optimization phase time (sec)", (times[7]));
        doc.get("User Optimization Overheads")->add("Optimization phase time vs reference SpMV+MG time", times[7] / times[8]);

        doc.add("DDOT Timing Variations", "");
        doc.get("DDOT Timing Variations")->add("Min DDOT MPI_Allreduce time", t4min);
        doc.get("DDOT Timing Variations")->add("Max DDOT MPI_Allreduce time", t4max);
        doc.get("DDOT Timing Variations")->add("Avg DDOT MPI_Allreduce time", t4avg);

        //doc.get("Sparse Operations Overheads")->add("Halo exchange time (sec)", (times[6]));
        //doc.get("Sparse Operations Overheads")->add("Halo exchange as percentage of SpMV time", (times[6])/totalSparseMVTime*100.0);
        doc.add("__________ Final Summary __________", "");
        bool isValidRun = (testcg_data.count_fail == 0) && (testsymmetry_data.count_fail == 0) && (testnorms_data.pass) && (!global_failure);
        if (isValidRun) {
            doc.get("__________ Final Summary __________")->add("HPCG result is VALID with a GFLOP/s rating of", totalGflops);
            doc.get("__________ Final Summary __________")->add("    HPCG 2.4 Rating (for historical value) is", totalGflops24);
            if (!A.isDotProductOptimized) {
                doc.get("__________ Final Summary __________")->add("Reference version of ComputeDotProduct used", "Performance results are most likely suboptimal");
            }
            if (!A.isSpmvOptimized) {
                doc.get("__________ Final Summary __________")->add("Reference version of ComputeSPMV used", "Performance results are most likely suboptimal");
            }
            if (!A.isMgOptimized) {
                if (Ageom->numThreads > 1) {
                    doc.get("__________ Final Summary __________")->add("Reference version of ComputeMG used and number of threads greater than 1", "Performance results are severely suboptimal");
                }
                else { // numThreads ==1
                    doc.get("__________ Final Summary __________")->add("Reference version of ComputeMG used", "Performance results are most likely suboptimal");
                }
            }
            if (!A.isWaxpbyOptimized) {
                doc.get("__________ Final Summary __________")->add("Reference version of ComputeWAXPBY used", "Performance results are most likely suboptimal");
            }
            if (times[0] >= minOfficialTime) {
                doc.get("__________ Final Summary __________")->add("Please upload results from the YAML file contents to", "http://hpcg-benchmark.org");
            }
            else {
                doc.get("__________ Final Summary __________")->add("Results are valid but execution time (sec) is", times[0]);
                if (quickPath) {
                    doc.get("__________ Final Summary __________")->add("     You have selected the QuickPath option", "Results are official for legacy installed systems with confirmation from the HPCG Benchmark leaders.");
                    doc.get("__________ Final Summary __________")->add("     After confirmation please upload results from the YAML file contents to", "http://hpcg-benchmark.org");

                }
                else {
                    doc.get("__________ Final Summary __________")->add("     Official results execution time (sec) must be at least", minOfficialTime);
                }
            }
        }
        else {
            doc.get("__________ Final Summary __________")->add("HPCG result is", "INVALID.");
            doc.get("__________ Final Summary __________")->add("Please review the YAML file contents", "You may NOT submit these results for consideration.");
        }

        std::string yaml = doc.generateYAML();
#ifdef HPCG_DEBUG
        HPCG_fout << yaml;
#endif
    }
    return;
}