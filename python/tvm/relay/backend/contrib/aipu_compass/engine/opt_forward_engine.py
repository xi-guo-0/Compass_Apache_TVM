# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2023-2024 Arm Technology (China) Co. Ltd.
"""Execute AIPU compass function through AIPU Optimizer."""
import os
import numpy as np
from tvm import nd
from .engine import PurePythonForwardEngine, FunctionData
from ..aipu_builder import check_call_aipu_tool, OptForward
from ..codegen import CodeGenAipuCompass
from ..config import AipuCompassConfig, AipuCompassFunctionConfig
from ..utils import relative_symlink_in_dir


class AIPUOptForwardEngine(PurePythonForwardEngine):
    """Build AIPU Compass function for running through AIPU Optimizer."""

    def __init__(self, is_quant):
        super().__init__()
        self._is_quant = is_quant

    def run(self, func_data, args):
        """Responsible for executing AIPU Compass function during Relay run the
        whole compiled model."""
        np_args = [x.numpy() for x in args]
        if self._is_quant:
            ret = func_data.executor.forward_with_quantized_data(np_args, False)

            # Dump all quantized input and output data for debugging with tool like "aipurun".
            if AipuCompassConfig.get().runtime["dump"] == "true":
                cfg = AipuCompassFunctionConfig(func_data.name)
                os.makedirs(cfg.gbuilder_work_dir, exist_ok=True)

                for i, np_arg in enumerate(np_args):
                    np_arg.tofile(f"{cfg.gbuilder_work_dir}/input{i}_{np_arg.dtype}.bin")
                for i, np_ret in enumerate(ret):
                    np_ret.tofile(f"{cfg.gbuilder_work_dir}/output{i}_{np_ret.dtype}.bin")
        else:
            ret = func_data.executor.forward(np_args, False)

        return ret

    def pre_build(self, func):
        """Build the AIPU Compass function to run on AIPU Optimizer before
        Relay build the whole model."""
        # Get the Compass IR from Relay IR.
        cfg = AipuCompassFunctionConfig(func.attrs.global_symbol)
        CodeGenAipuCompass().gen2file(func, *cfg.compass_ir_path)
        ir_path = cfg.compass_ir_path

        # Simplify Compass float IR through "aipugsim" if needed.
        if cfg.use_gsim_float:
            # Create symbolic links that point to Compass IR in AIPU gsim_float directory.
            relative_symlink_in_dir(cfg.compass_ir_path, cfg.gsim_float_work_dir)
            check_call_aipu_tool(cfg.gsim_cmd("float"), cfg.gsim_float_work_dir)
            ir_path = cfg.gsim_float_ir_path

        # Get the quantized Compass IR through AIPU Optimizer if needed.
        if self._is_quant:
            # Create symbolic links that point to Compass IR in AIPU Optimizer directory.
            float_ir_path = cfg.gsim_float_ir_path if cfg.use_gsim_float else cfg.compass_ir_path
            relative_symlink_in_dir(float_ir_path, cfg.optimizer_work_dir)
            check_call_aipu_tool(cfg.optimizer_cmd, cfg.optimizer_work_dir)
            ir_path = cfg.quant_compass_ir_path

        # Read back the Compass IR.
        txt_path, bin_path = ir_path
        ir_txt = open(txt_path, "r", encoding="utf-8").read()
        ir_bin = np.fromfile(bin_path, dtype="uint8")

        try:
            # Try to obtain the NDArray object without memory copy.
            ir_bin = nd.from_dlpack(ir_bin)
        except:  # pylint: disable=bare-except
            ir_bin = nd.array(ir_bin)

        # Embed the pre-build result into Relay IR through attribute of function.
        new_attrs = {"compass.pre_build.ir_txt": ir_txt, "compass.pre_build.ir_bin": ir_bin}
        return func.with_attr(new_attrs)

    def create_func_data(self, func):
        """Responsible for creating object used to store data of the Relay
        function with the pre-build result during Relay build the whole model."""
        func_data = FunctionData(func)

        # Write the Compass IR to disk for AIPU Optimizer forward interface.
        cfg = AipuCompassFunctionConfig(func_data.name)
        txt_path, bin_path = cfg.compass_ir_path
        if self._is_quant:
            txt_path, bin_path = cfg.quant_compass_ir_path

        # Get the pre-build result back from Relay IR.
        # There is a bug in Relay parser, it won't resume the escaped characters.
        ir_txt = func.attrs["compass.pre_build.ir_txt"].encode("utf-8").decode("unicode_escape")
        ir_bin = func.attrs["compass.pre_build.ir_bin"].numpy()

        os.makedirs(os.path.dirname(txt_path), exist_ok=True)
        open(txt_path, "w", encoding="utf-8").write(ir_txt)
        os.makedirs(os.path.dirname(bin_path), exist_ok=True)
        ir_bin.tofile(bin_path)

        # Create the AIPU Optimizer forward instance.
        func_data.executor = OptForward(txt_path, bin_path)
        return func_data
