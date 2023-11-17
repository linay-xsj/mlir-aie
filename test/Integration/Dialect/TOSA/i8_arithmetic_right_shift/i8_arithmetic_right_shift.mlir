// REQUIRES: valid_xchess_license
// RUN: aie-opt %s %tosa-to-linalg% | aie-opt %linalg-to-vector-v64% --convert-vector-to-aievec="aie-target=aieml" -lower-affine | aie-translate -aieml=true --aievec-to-cpp -o dut.cc
// RUN: xchesscc_wrapper aie2 -f -g +s +w work +o work -I%S -I %aietools/include -D__AIEARCH__=20 -D__AIENGINE__ -I. %S/testbench.cc dut.cc
// RUN: mkdir -p data
// RUN: xca_udm_dbg --aiearch aie-ml -qf -T -P %aietools/data/aie_ml/lib/ -t "%S/../profiling.tcl ./work/a.out" >& xca_udm_dbg.stdout
// RUN: FileCheck --input-file=./xca_udm_dbg.stdout %s
// CHECK: TEST PASSED
// Cycle count: 59

func.func @dut(%arg0: tensor<1024xi8>, %arg1: tensor<1xi8>) -> (tensor<1024xi8>) {
  %0 = "tosa.arithmetic_right_shift"(%arg0, %arg1) {round = 0 : i1} : (tensor<1024xi8>, tensor<1xi8>) -> tensor<1024xi8>
  return %0 : tensor<1024xi8>
}
