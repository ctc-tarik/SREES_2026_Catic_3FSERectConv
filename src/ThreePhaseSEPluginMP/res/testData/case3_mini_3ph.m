function mpc = case3_mini_3ph
% case3_mini_3ph - tiny 3-bus, perfectly balanced three-phase test case.
% Used to hand-verify the polar -> rectangular conversion:
%   bus 1, phase A: Vm=1.00 Va=0deg     -> Vr=1.00000000  Vi=0.00000000
%   bus 1, phase B: Vm=1.00 Va=-120deg  -> Vr=-0.50000000 Vi=-0.86602540
%   bus 1, phase C: Vm=1.00 Va=120deg   -> Vr=-0.50000000 Vi=0.86602540

mpc.version = '2';
mpc.baseMVA = 100;

% Standard (single-phase / positive-sequence) Matpower bus table, kept for
% realism so the file still loads as a valid Matpower case. Not used by
% the ThreePhaseSEPluginMP converter.
% bus_i type Pd Qd Gs Bs area Vm Va baseKV zone Vmax Vmin
mpc.bus = [
    1  3  0  0  0  0  1  1.00  0.0  138  1  1.10  0.90;
    2  1  0  0  0  0  1  1.00  0.0  138  1  1.10  0.90;
    3  1  0  0  0  0  1  1.00  0.0  138  1  1.10  0.90;
];

% Three-phase extension used by ThreePhaseSEPluginMP.
% Columns: busId Vm_A Va_A Vm_B Va_B Vm_C Va_C  (Vm in p.u., Va in degrees)
mpc.bus3ph = [
      1   1.00000     0.0000    1.00000  -120.0000    1.00000   120.0000;
      2   0.98000     0.0000    0.98000  -120.0000    0.98000   120.0000;
      3   0.95000     0.0000    0.95000  -120.0000    0.95000   120.0000;
];

end
