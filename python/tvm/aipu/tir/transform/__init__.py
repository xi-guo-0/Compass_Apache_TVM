# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2023-2024 Arm Technology (China) Co. Ltd.
"""Zhouyi Compass extension of TIR transform passes."""
from .transform import *
from .gen_buffer_stride import GenBufferStride
from .add_dma_pragma import AddDMAPragma
from .add_likely_for_loop_partition import AddLikelyForLoopPartition
from .align_param_var_with_buffer import AlignParamVarWithBuffer
from .revise_param_r import ReviseParamR
from .precodegen import Precodegen
from .lower_pred import LowerPred
from .reassign_var_by_0dim_buffer import ReassignVarBy0DimBuffer
from .reassign_var_by_let import ReassignVarByLet
from .substitute_size_var import SubstituteSizeVar
from .remove_if_in_vec_for import RemoveIfInVecFor
from .uniquify_var_name import UniquifyVarName
from .rename_for_loop_var import RenameForLoopVar
from .canonicalize_ramp import CanonicalizeRamp
from .common_get_local_id_eliminate import EliminateGetLocalID
from .rename_alloc_const_buffer_var import RenameConstBufferVar
from .simplify_buf_realize import BufRealizeSimplifier
from .canonicalize_div_mod import CanonicalizeDivMod
from .rename_utils_fname import RenameUtilsFnames
from .lower_standard import LowerStandard
from .align_vector_width_by_split import AlignVectorWidthBySplit
from .align_vector_width_by_pad import AlignVectorWidthByPad
from .revert2standard import Revert2Standard
from .fold_constant import FoldConstant
from .lower_virtual_isa import LowerVirtualIsa
from .lower_vector_cast import LowerVectorCast
from .simplify import Simplify
from .lower_virtual_vector_pointer import LowerVirtualVectorPointer
from .initialize_event_state import InitializeEventState
from .exchange_constant_to_right import ExchangeConstantToRight
from .combine_instructions import CombineInstructions
from .isa_aware_rewrite import IsaAwareRewrite
from .merge_for_where import MergeForWhere
