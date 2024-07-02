# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2023-2024 Arm Technology (China) Co. Ltd.
"""Common AIPU utilities."""
import os
import re
import operator
import functools
from subprocess import run, STDOUT
import numpy as np
import tvm
from .. import autotvm, contrib, rpc, tir, DataType
from .logger import INFO


_EXE_NAME2TOOL_NAME = {
    "aipuopt": "Optimizer",
    "aipugb": "GBuilder",
    "aipugsim": "GSim",
    "aipurun": "AIPURun",
    "aipu_profiler": "Profiler",
}


def check_call_aipu_tool(cmd, work_dir=os.getcwd()):
    """Call tools of AIPUBuilder through sub process and check the return code."""
    work_dir = os.path.abspath(work_dir)
    old_cwd = os.getcwd()
    if work_dir != old_cwd:
        os.makedirs(work_dir, exist_ok=True)
        os.chdir(work_dir)

    exe_name = cmd[0]
    log_file = f"{work_dir}/{exe_name}.log"
    with open(log_file, "w", encoding="utf-8") as f:
        f.write(f"Command Line: {' '.join(cmd)}\n")
        f.flush()
        env = None
        if exe_name == "aipuopt":
            # Workaround for the slow OPT on Python3.8.5 CPU environment.
            env = dict(os.environ)
            env["OMP_NUM_THREADS"] = "4"

        ret_code = run(
            cmd,
            stdout=f,
            stderr=STDOUT,
            check=False,
            encoding="utf-8",
            env=env,
            text=True,
        ).returncode

    count_errors = 0
    with open(log_file, "r", encoding="utf-8") as f:
        error_pattern = re.compile(r"(?<=Total errors: )\d+")
        for line in f.readlines():
            digit_list = error_pattern.findall(line)
            if len(digit_list) == 0:
                continue
            for digit in digit_list:
                if int(digit) > 0:
                    count_errors = int(digit)
                    break
            if count_errors != 0:
                break

    if old_cwd != os.getcwd():
        os.chdir(old_cwd)

    if ret_code != 0 or count_errors != 0:
        raise RuntimeError(
            f"Error happened when executing the AIPU {_EXE_NAME2TOOL_NAME[exe_name]}, for more "
            f'details, please refer to the log file "{log_file}".'
        )


def abspath(path, base_dir=None):
    """Return the absolute path of the given path and the base directory.

    Parameters
    ----------
    path : Optional[str]
        The given path.

    base_dir : Optional[str]
        The base directory will be used only when the given path is a relative
        one, if it is None, the current working directory will be used.

    Return
    ------
    ret : Optional[str]
        The result absolute path. None if the given path is None.
    """
    if path is None:
        return None

    path = os.path.expandvars(os.path.expanduser(path))
    if os.path.isabs(path):
        # "abspath" here is used to remove "." and "..".
        path = os.path.abspath(path)
    else:
        path = os.path.abspath(f"{base_dir or os.getcwd()}/{path}")
    return path


def get_rpc_session(
    session_timeout=600, rpc_key=None, tracker_host=None, tracker_port=None, priority=1
):
    """Connect to the RPC tracker and get a RPC session with the RPC key.

    Parameters
    ----------
    session_timeout : Optional[float]
        The duration of the session, allows server to kill
        the connection when duration is longer than this value.
        When duration is zero, it means the request must always be kept alive.

    rpc_key : Optional[str]
        The type key of the device(default=None).
        If rpc_key = "None", get it from env "AIPU_TVM_RPC_KEY".

    tracker_host : Optional[str]
        The hostname or IP address of RPC tracker(default = None).
        If tracker_host = "None", get it from env "AIPU_TVM_RPC_TRACKER_IP".

    tracker_port: Optional[int, str]
        The port of PRC tracker(default = None)
        If tracker_port = "None", get it from env "AIPU_TVM_RPC_TRACKER_PORT".

    priority : Optional[int]
        The priority of the request(default=1).
        If priority = "None", get it from env "AIPU_TVM_RPC_PRIORITY".

    Returns
    -------
    sess : tvm.rpc.RPCSession
        The RPC session that is already connected to the RPC server.
    """
    # Override logic of RPC key is special, function argument has higher priority.
    rpc_key = rpc_key or os.getenv("AIPU_TVM_RPC_KEY")
    assert rpc_key, 'Set RPC key through arg or env "AIPU_TVM_RPC_KEY".'

    tracker_host = os.getenv("AIPU_TVM_RPC_TRACKER_IP") or tracker_host
    assert tracker_host, 'Set RPC tracker host through arg or env "AIPU_TVM_RPC_TRACKER_IP".'
    tracker_port = os.getenv("AIPU_TVM_RPC_TRACKER_PORT") or tracker_port
    assert tracker_port, 'Set RPC tracker port through arg or env "AIPU_TVM_RPC_TRACKER_PORT".'
    priority = os.getenv("AIPU_TVM_RPC_PRIORITY") or priority
    assert priority, 'Set RPC priority through arg or env "AIPU_TVM_RPC_PRIORITY".'

    valid_rpc_keys = os.getenv("AIPU_TVM_VALID_RPC_KEYS")
    if valid_rpc_keys:
        valid_rpc_keys = tuple(x.strip() for x in valid_rpc_keys.split("|") if x.strip() != "")
        assert (
            rpc_key in valid_rpc_keys
        ), f"Invalid RPC key '{rpc_key}', the valid choices are {valid_rpc_keys}."

    return rpc.connect_tracker(tracker_host, int(tracker_port)).request(
        key=rpc_key, priority=int(priority), session_timeout=session_timeout
    )


def check_remote(rpc_key=None, tracker_host=None, tracker_port=None):
    """Check the remote device is available or not."""
    pool = contrib.popen_pool.PopenPoolExecutor(max_workers=1, timeout=10)

    def _check():
        get_rpc_session(5, rpc_key, tracker_host, tracker_port, 100)

    try:
        pool.submit(_check).result()
    except TimeoutError:
        return False
    return True


def prod_const(arr):
    """Reduce product the given input sequence to a constant value."""
    const_arr = []
    for x in arr:
        if isinstance(x, tir.IterVar):
            x = x.dom.extent
        const_arr.append(autotvm.utils.get_const_int(x))

    return functools.reduce(operator.mul, const_arr, 1)


def canonicalize_target(target):
    """Canonicalize target and return tvm.target.Target."""
    if isinstance(target, tvm.target.Target):
        return target
    assert isinstance(target, str), f"Unsupported target type: {type(target)}."
    if not target.startswith("aipu"):
        target = "aipu -mcpu=" + target
    return tvm.target.Target(target)


def vec_type(dtype):
    """Get the corresponding vector version of the given data type."""
    scalar_dtype = DataType(dtype)
    return scalar_dtype.with_lanes(256 // scalar_dtype.bits)


_DTYPE2RANGE = {"bool": (0, 1)}
for _dtype in ("uint8", "int8", "uint16", "int16", "uint32", "int32", "float16", "float32"):
    _range = (np.finfo if DataType(_dtype).is_float else np.iinfo)(_dtype)
    _DTYPE2RANGE[_dtype] = (_range.min, _range.max)


def get_range(dtype):
    """Get the minimum and maximum value of the given data type."""
    return _DTYPE2RANGE[DataType(dtype).element_of]


# Don't set value here, set it through environment variable "AIPU_TVM_RANDOM_SEED".
_RANDOM_SEED = None


def rand(shape, dtype, low=None, high=None, enable_corner_values=True):
    """Random values in a given shape, dtype and [low, high) range (includes low, excludes high).

    Parameters
    ----------
    shape : Union[int,Tuple[int],List[int]]
        The element number will rand

    dtype : str
        The data type

    low : Optional[int,float]
        The maximum threshold for rand range, default is None

    high : Optional[int,float]
        The minimum threshold for rand range, default is None

    enable_corner_values: Optional[bool]
        Whether the corner values are forced included or not, default is True. Note:
        1. The corner values contains: low or dtype minimum value, high or dtype maximum value,
        zero value when zero in random range;
        2. When value is True and elements number is less than corner values' number, it's
        uncertain that corner values are forced included: whether corner values are existed
        depends on randomness.

    Returns
    -------
    out: Union[float,int,numpy.ndarray]
        Rand values, scalar when shape is 1 or numpy.ndarray when shape is tuple of int

    Examples
    --------
    .. code-block:: python

        a = rand(100,"int8")
        b = rand((4,16), "float16", low=-100, high=100)

    """
    global _RANDOM_SEED
    if _RANDOM_SEED is None:
        _RANDOM_SEED = os.getenv("AIPU_TVM_RANDOM_SEED") or np.random.randint(0, 2**31)
        np.random.seed(int(_RANDOM_SEED))
        INFO(f'The NumPy random seed is "{_RANDOM_SEED}".')

    dtype = str(dtype)
    if dtype == "bool":
        out = np.random.uniform(size=shape) < 0.5
        return out[0] if shape == 1 else out

    if dtype.startswith("float"):
        dtype = "float32" if dtype == "float64" else dtype
        np_dtype_info = np.finfo(dtype)
        max_eps = 1e-5
        rand_func = lambda low, high: np.random.uniform(low, high, shape).astype(dtype)
    else:
        np_dtype_info = np.iinfo(dtype)
        max_eps = 1
        rand_func = lambda low, high: np.random.randint(low, high, shape, dtype)

    minv = np_dtype_info.min if low is None else low
    maxv = np_dtype_info.max if high is None else high - max_eps
    if minv == maxv:
        return getattr(np, dtype)(minv) if shape == 1 else np.full(shape, minv, dtype=dtype)

    assert minv < maxv
    out = rand_func(minv, maxv)

    corner_values = (minv, 0, maxv) if minv < 0 < maxv else (minv, maxv)
    if enable_corner_values and out.size > len(corner_values):
        occupied_indices = []
        for val in corner_values:
            idx = tuple(np.random.randint(out.shape))
            while idx in occupied_indices:
                idx = tuple(np.random.randint(out.shape))

            out[idx] = val
            occupied_indices.append(idx)

    return out[0] if shape == 1 else out


def double_elem_width(vdtype, allow_64bit=False):
    if not allow_64bit and vdtype.bits == 32:
        return vdtype

    new_bits = vdtype.bits * 2
    # For the type u8x8, the result type should be u16x8 instead of u16x4.
    new_lanes = vdtype.lanes
    if new_bits * new_lanes > 256:
        new_lanes //= 2
    return vdtype.with_bits(new_bits).with_lanes(new_lanes)


def half_elem_width(vdtype, double_lanes=True):
    assert vdtype.bits >= 8

    # For the type u16x8, maybe the expect result type is u8x8 instead of u8x16.
    new_lanes = vdtype.lanes * 2 if double_lanes else vdtype.lanes
    return vdtype.with_bits(vdtype.bits // 2).with_lanes(new_lanes)
