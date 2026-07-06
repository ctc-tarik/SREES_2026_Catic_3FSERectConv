function mpc = case9_3ph
% case9_3ph - 9-bus three-phase test case (mild voltage unbalance),
% extended from the classic Matpower 9-bus topology for
% ThreePhaseSEPluginMP.

mpc.version = '2';
mpc.baseMVA = 100;

% Simplified single-phase bus table (illustrative; not used by the converter).
% bus_i type Pd Qd Gs Bs area Vm Va baseKV zone Vmax Vmin
mpc.bus = [
    1  3  0     0    0  0  1  1.00  0.0  230  1  1.10  0.90;
    2  2  0     0    0  0  1  1.00  0.0  230  1  1.10  0.90;
    3  2  0     0    0  0  1  1.00  0.0  230  1  1.10  0.90;
    4  1  0     0    0  0  1  1.00  0.0  230  1  1.10  0.90;
    5  1  90    30   0  0  1  1.00  0.0  230  1  1.10  0.90;
    6  1  0     0    0  0  1  1.00  0.0  230  1  1.10  0.90;
    7  1  100   35   0  0  1  1.00  0.0  230  1  1.10  0.90;
    8  1  0     0    0  0  1  1.00  0.0  230  1  1.10  0.90;
    9  1  125   50   0  0  1  1.00  0.0  230  1  1.10  0.90;
];

% Three-phase extension used by ThreePhaseSEPluginMP.
% Columns: busId Vm_A Va_A Vm_B Va_B Vm_C Va_C  (Vm in p.u., Va in degrees)
mpc.bus3ph = [
      1   0.98778     0.0330    0.98240  -122.4678    0.96831   120.0139;
      2   1.01390    -1.1417    0.96485  -122.2955    0.99326   119.3951;
      3   1.00221    -3.0499    0.98712  -121.3099    1.00350   118.0547;
      4   0.99038    -1.1903    1.01543  -120.5817    0.99543   121.2305;
      5   0.96610    -1.6600    0.97747  -120.3912    1.00045   117.9386;
      6   0.97600    -1.6667    0.97259  -121.5026    0.97687   121.9320;
      7   1.01276    -3.0790    0.96329  -122.3826    0.98273   119.6236;
      8   1.01438    -4.6757    0.96672  -119.6065    0.99581   121.9765;
      9   0.97218    -3.5108    0.96050  -122.4127    0.96501   117.9242;
];

end
