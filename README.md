# Shallow Water Simulation

![Dam break simulation](https://github.com/cs5220-f20/shallow-water/blob/main/img/dam_break.gif)

Running the Shallow Water Simulation:

To perform our experiments, we modified the arguments for the simulator. 

Here is how to run the simulator without running the scaling experiments:

`src/lshallow tests.lua NAME NY N`, where `N` is the number of threads to use during a normal run.

To run the scaling experiments, simply run
`src/lshallow tests.lua NAME NY`. If the number of threads isn't provided, we assume that you plan on running the strong and weak scaling experiments. `NY` is the value for `ny` used at the beginning of the experiments.

The scaling experiments aren't configurable without modifying code. Currently, they iterate between `1` and `32` threads. For the weak scaling experiments,
the number of threads and `ny` doubles each iteration.

If you plan on running the scaling experiments in Graphite, we provide a `experiments.sub` file that's configured to run on the dam break simulation with `NY = 1000`.
We request exclusive access to `4` nodes in Graphite as well.
