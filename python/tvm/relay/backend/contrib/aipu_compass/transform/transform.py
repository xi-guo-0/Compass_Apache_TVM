# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2023-2024 Arm Technology (China) Co. Ltd.
# pylint: disable=invalid-name
"""The AIPU Compass extended Relay transform passes."""
from tvm import relay


def PrimalLayoutTransformToTranspose():
    """convert the primal layout_transform op to transpose op

    Returns
    -------
    ret : tvm.transform.Pass
        The registered pass for primal layout_transform to transpose.
    """
    return relay.transform._ffi_api.PrimalLayoutTransformToTranspose()


def ReAnnotateTuple():
    """Set the tuple annotation target to be the same as its user.

    Returns
    -------
    ret : tvm.transform.Pass
        The registered pass for reannotate tuple target.
    """
    return relay.transform._ffi_api.ReAnnotateTuple()
