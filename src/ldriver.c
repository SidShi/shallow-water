#include "stepper.h"
#include "shallow2d.h"

#ifdef _OPENMP
#include <omp.h>
#elif defined SYSTIME
#include <sys/time.h>
#endif

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <assert.h>
#include <stdio.h>
#include <gperftools/profiler.h>

//ldoc on
/**
 * # Driver code
 *
 * The driver code is where we put together the time stepper and
 * the physics routines to actually solve the equations and make
 * pretty pictures of the solutions.
 *
 * ## Diagnostics
 *
 * The numerical method is supposed to preserve (up to rounding
 * errors) the total volume of water in the domain and the total
 * momentum.  Ideally, we should also not see negative water heights,
 * since that will cause the system of equations to blow up.  For
 * debugging convenience, we'll plan to periodically print diagnostic
 * information about these conserved quantities (and about the range
 * of water heights).
 */

void solution_check(central2d_t *sim)
{
    int nx = sim->nx, ny = sim->ny;
    float *u = sim->u;
    float h_sum = 0, hu_sum = 0, hv_sum = 0;
    float hmin = u[central2d_offset(sim, 0, 0, 0)];
    float hmax = hmin;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
        {
            float h = u[central2d_offset(sim, 0, i, j)];
            h_sum += h;
            hu_sum += u[central2d_offset(sim, 1, i, j)];
            hv_sum += u[central2d_offset(sim, 2, i, j)];
            hmax = fmaxf(h, hmax);
            hmin = fminf(h, hmin);
        }
    float cell_area = sim->dx * sim->dy;
    h_sum *= cell_area;
    hu_sum *= cell_area;
    hv_sum *= cell_area;
    printf("-\n  Volume: %g\n  Momentum: (%g, %g)\n  Range: [%g, %g]\n",
           h_sum, hu_sum, hv_sum, hmin, hmax);
    assert(hmin > 0);
}

/**
 * ## I/O
 *
 * After finishing a run (or every several steps), we might want to
 * write out a data file for further processing by some other program
 * -- in this case, a Python visualizer.  The visualizer takes the
 * number of pixels in x and y in the first two entries, then raw
 * single-precision raster pictures.
 */

FILE *viz_open(const char *fname, central2d_t *sim, int vskip)
{
    FILE *fp = fopen(fname, "w");
    if (fp)
    {
        float xy[2] = {sim->nx / vskip, sim->ny / vskip};
        fwrite(xy, sizeof(float), 2, fp);
    }
    return fp;
}

void viz_close(FILE *fp)
{
    fclose(fp);
}

void viz_frame(FILE *fp, central2d_t *sim, int vskip)
{
    if (!fp)
        return;
    for (int iy = 0; iy < sim->ny; iy += vskip)
        for (int ix = 0; ix < sim->nx; ix += vskip)
            fwrite(sim->u + central2d_offset(sim, 0, ix, iy),
                   sizeof(float), 1, fp);
}

/**
 * ## Lua driver routines
 *
 * A better way to manage simulation parameters is by a scripting
 * language.  Python is a popular choice, but I prefer Lua for many
 * things (not least because it is an easy build).  It's also quite
 * cheap to call a Lua function for every point in a mesh
 * (less so for Python, though it probably won't make much difference).
 *
 * ### Lua callback functions
 *
 * We specify the initial conditions by providing the simulator
 * with a callback function to be called at each cell center.
 * The callback function is assumed to be the `init` field of
 * a table at index 1.
 */

void lua_init_sim(lua_State *L, central2d_t *sim)
{
    lua_getfield(L, 1, "init");
    if (lua_type(L, -1) != LUA_TFUNCTION)
        luaL_error(L, "Expected init to be a string");

    int nx = sim->nx, ny = sim->ny, nfield = sim->nfield;
    float dx = sim->dx, dy = sim->dy;
    float *u = sim->u;

    for (int ix = 0; ix < nx; ++ix)
    {
        float x = (ix + 0.5) * dx;
        for (int iy = 0; iy < ny; ++iy)
        {
            float y = (iy + 0.5) * dy;
            lua_pushvalue(L, -1);
            lua_pushnumber(L, x);
            lua_pushnumber(L, y);
            lua_call(L, 2, nfield);
            for (int k = 0; k < nfield; ++k)
                u[central2d_offset(sim, k, ix, iy)] = lua_tonumber(L, k - nfield);
            lua_pop(L, nfield);
        }
    }

    lua_pop(L, 1);
}

/**
 * ### Running the simulation
 *
 * The `run_sim` function looks a lot like the main routine of the
 * "ordinary" command line driver.  We specify the initial conditions
 * by providing the simulator with a callback function to be called at
 * each cell center.  Note that we have two different options for
 * timing the steps -- we can use the OpenMP timing routines
 * (preferable if OpenMP is available) or the POSIX `gettimeofday`
 * if the `SYSTIME` macro is defined.  If there's no OpenMP and
 * `SYSTIME` is undefined, we fall back to just printing the number
 * of steps without timing information.
 */

int run_sim(lua_State *L)
{
    int n = lua_gettop(L);
    if (n != 1 || !lua_istable(L, 1))
        luaL_error(L, "Argument must be a table");

    lua_getfield(L, 1, "w");
    lua_getfield(L, 1, "h");
    lua_getfield(L, 1, "cfl");
    lua_getfield(L, 1, "ftime");
    lua_getfield(L, 1, "nx");
    lua_getfield(L, 1, "ny");
    lua_getfield(L, 1, "vskip");
    lua_getfield(L, 1, "frames");
    lua_getfield(L, 1, "out");
    lua_getfield(L, 1, "threads");

    double w = luaL_optnumber(L, 2, 2.0);
    double h = luaL_optnumber(L, 3, w);
    double cfl = luaL_optnumber(L, 4, 0.45);
    double ftime = luaL_optnumber(L, 5, 0.01);
    int nx = luaL_optinteger(L, 6, 200);
    int ny = luaL_optinteger(L, 7, nx);
    int vskip = luaL_optinteger(L, 8, 1);
    int frames = luaL_optinteger(L, 9, 50);
    const char *fname = luaL_optstring(L, 10, "sim.out");
    int threads = luaL_optinteger(L, 11, -1);
    lua_pop(L, 9);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("%i\n",threads);
    if (threads == -1)
    {
        ProfilerStart("shallow.prof");
        printf("Begin strong scaling\n");
        for (int threads = 1; threads <= 8; threads *= 2)
        {
            double avg_time = 0.0;
            for (int k = 0; k < 3; k++)
            {
                central2d_t *sim = central2d_init(w, h, nx, ny,
                                                  3, shallow2d_flux, shallow2d_speed, cfl);
                lua_init_sim(L, sim);
                // printf("%g %g %d %d %g %d %g\n", w, h, nx, ny, cfl, frames, ftime);
                //FILE* viz = viz_open(fname, sim, vskip);
                //solution_check(sim);
                //viz_frame(viz, sim, vskip);

                double tcompute = 0;
                for (int i = 0; i < frames; ++i)
                {
#ifdef _OPENMP
                    double t0 = omp_get_wtime();
                    int nstep = central2d_run(sim, ftime, threads);
                    double t1 = omp_get_wtime();
                    double elapsed = t1 - t0;
#elif defined SYSTIME
                    struct timeval t0, t1;
                    gettimeofday(&t0, NULL);
                    int nstep = central2d_run(sim, ftime, threads);
                    gettimeofday(&t1, NULL);
                    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) * 1e-6;
#else
                    int nstep = central2d_run(sim, ftime, threads);
                    double elapsed = 0;
#endif
                    // solution_check(sim);
                    tcompute += elapsed;
                    //printf("  Time: %e (%e for %d steps)\n", elapsed, elapsed/nstep, nstep);
                    //viz_frame(viz, sim, vskip);
                }
                avg_time += tcompute;

                //viz_close(viz);
                central2d_free(sim);
            }
            avg_time /= 3.0;
            printf("Threads %i: Height: %i, Average compute time: %e\n", threads, ny, avg_time);
        }
        printf("Begin weak scaling\n");
        for (int threads = 1; threads <= 8; threads *= 2)
        {
            double avg_time = 0.0;
            for (int k = 0; k < 3; k++)
            {
                central2d_t *sim = central2d_init(w, h, nx, ny,
                                                  3, shallow2d_flux, shallow2d_speed, cfl);
                lua_init_sim(L, sim);
                // printf("%g %g %d %d %g %d %g\n", w, h, nx, ny, cfl, frames, ftime);
                //FILE* viz = viz_open(fname, sim, vskip);
                //solution_check(sim);
                //viz_frame(viz, sim, vskip);

                double tcompute = 0;
                for (int i = 0; i < frames; ++i)
                {
#ifdef _OPENMP
                    double t0 = omp_get_wtime();
                    int nstep = central2d_run(sim, ftime, threads);
                    double t1 = omp_get_wtime();
                    double elapsed = t1 - t0;
#elif defined SYSTIME
                    struct timeval t0, t1;
                    gettimeofday(&t0, NULL);
                    int nstep = central2d_run(sim, ftime, threads);
                    gettimeofday(&t1, NULL);
                    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) * 1e-6;
#else
                    int nstep = central2d_run(sim, ftime, threads);
                    double elapsed = 0;
#endif
                    //solution_check(sim);
                    tcompute += elapsed;
                    //printf("  Time: %e (%e for %d steps)\n", elapsed, elapsed/nstep, nstep);
                    //viz_frame(viz, sim, vskip);
                }
                avg_time += tcompute;

                //viz_close(viz);
                central2d_free(sim);
            }
            avg_time /= 3.0;
            printf("Threads %i: Height: %i, Average compute time: %e\n", threads, ny, avg_time);
            ny *= 2;
        }
        ProfilerStop();
    }
    else
    {
        central2d_t *sim = central2d_init(w, h, nx, ny,
                                          3, shallow2d_flux, shallow2d_speed, cfl);
        lua_init_sim(L, sim);
        printf("%g %g %d %d %g %d %g\n", w, h, nx, ny, cfl, frames, ftime);
        FILE *viz = viz_open(fname, sim, vskip);
        solution_check(sim);
        viz_frame(viz, sim, vskip);

        double tcompute = 0;
        for (int i = 0; i < frames; ++i)
        {
#ifdef _OPENMP
            double t0 = omp_get_wtime();
            int nstep = central2d_run(sim, ftime, threads);
            double t1 = omp_get_wtime();
            double elapsed = t1 - t0;
#elif defined SYSTIME
            struct timeval t0, t1;
            gettimeofday(&t0, NULL);
            int nstep = central2d_run(sim, ftime, threads);
            gettimeofday(&t1, NULL);
            double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) * 1e-6;
#else
            int nstep = central2d_run(sim, ftime, threads);
            double elapsed = 0;
#endif
            solution_check(sim);
            tcompute += elapsed;
            printf("  Time: %e (%e for %d steps)\n", elapsed, elapsed / nstep, nstep);
            viz_frame(viz, sim, vskip);

        }
        printf("Total compute time: %e\n", tcompute);
        central2d_free(sim);
        viz_close(viz);
    }
    return 0;
}

/**
 * ### Main
 *
 * The main routine has the usage pattern
 *
 *     lshallow tests.lua args
 *
 * where `tests.lua` has a call to the `simulate` function to run
 * the simulation.  The arguments after the Lua file name are passed
 * into the Lua script via a global array called `args`.
 */

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s fname args\n", argv[0]);
        return -1;
    }

    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lua_register(L, "simulate", run_sim);

    lua_newtable(L);
    for (int i = 2; i < argc; ++i)
    {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, 1, i - 1);
    }
    lua_setglobal(L, "args");

    if (luaL_dofile(L, argv[1]))
        printf("%s\n", lua_tostring(L, -1));
    lua_close(L);
    return 0;
}
