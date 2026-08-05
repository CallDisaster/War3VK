#!/usr/bin/env python3
"""GPU 蒙皮 VS 输入租约的可执行离线契约与静态接线审计。

本脚本不能证明 C++ 运行时已经正确执行，也绝不会启动《魔兽争霸 III》。它演练最保守的
消费者栅栏状态机，并只读审计当前 VS-A、VS-B0 与 VS-B1 是否采用 device-local
palette 副本、精确 Main/Shadow 门、同一命令内清零/解绑/恢复，以及 B0 的 P4
零权限和 B1 的不可回退 input-capability 边界。
构建、隔离运行、画面与性能仍是独立硬门。
"""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import dataclass, field, replace
from datetime import datetime
from pathlib import Path
from typing import Mapping


MAIN = 1 << 0
SHADOW = 1 << 1
OUTLINE = 1 << 2
PARITY = 1 << 3
KNOWN_CONSUMERS = MAIN | SHADOW | OUTLINE | PARITY

ROUTE_COMPUTE = "compute"
ROUTE_VERTEX_SHADER = "vertex_shader"
ROUTE_VERTEX_SHADER_INPUT_ONLY = "vertex_shader_input_only"
ROUTE_VERTEX_SHADER_BYPASS = "vertex_shader_bypass"


@dataclass(frozen=True)
class FenceReceipt:
    fence: str
    value: int

    def __post_init__(self) -> None:
        if not self.fence or self.value <= 0:
            raise ValueError("a fence receipt requires a name and positive value")


@dataclass(frozen=True)
class LeaseAccess:
    lease_id: int
    consumer_bit: int
    runtime_generation: int
    map_epoch: int
    device_epoch: int
    token: int
    page_id: int
    page_generation: int
    static_byte_offset: int
    static_byte_length: int
    palette_byte_offset: int
    palette_byte_length: int


@dataclass(frozen=True)
class SubmissionTicket:
    """不透明能力：提交操作只接受原样返回的那个对象。"""

    ticket_id: int
    lease_id: int
    consumer_bit: int
    runtime_generation: int
    map_epoch: int
    device_epoch: int
    token: int
    page_id: int
    page_generation: int
    static_byte_offset: int
    static_byte_length: int
    palette_byte_offset: int
    palette_byte_length: int


@dataclass
class PendingSubmission:
    ticket: SubmissionTicket
    irreversible: bool = True


@dataclass
class InputLease:
    lease_id: int
    runtime_generation: int
    map_epoch: int
    device_epoch: int
    token: int
    page_id: int
    page_generation: int
    static_byte_offset: int
    static_byte_length: int
    palette_byte_offset: int
    palette_byte_length: int
    requested_bits: int
    producer_receipt: FenceReceipt | None
    open_bits: int = 0
    pending_ticket_ids: dict[int, int] = field(default_factory=dict)
    gpu_receipts: dict[int, FenceReceipt] = field(default_factory=dict)
    no_gpu_bits: int = 0
    reset_cancelled_bits: int = 0
    retirement_queued: bool = False
    reclaimed: bool = False

    def __post_init__(self) -> None:
        self.open_bits = self.requested_bits

    @property
    def pending_bits(self) -> int:
        result = 0
        for bit in self.pending_ticket_ids:
            result |= bit
        return result

    @property
    def gpu_bits(self) -> int:
        result = 0
        for bit in self.gpu_receipts:
            result |= bit
        return result

    @property
    def terminal_bits(self) -> int:
        return self.gpu_bits | self.no_gpu_bits | self.reset_cancelled_bits


@dataclass
class UploadPage:
    page_id: int
    generation: int = 1
    current_lease_ids: list[int] = field(default_factory=list)
    live_claims: set[tuple[int, int]] = field(default_factory=set)
    reusable: bool = False
    reuse_count: int = 0


class InputLeaseModel:
    def __init__(self, map_epoch: int = 1, device_epoch: int = 1) -> None:
        if map_epoch <= 0 or device_epoch <= 0:
            raise ValueError("initial epochs must be positive")
        self.runtime_generation = 1
        self.map_epoch = map_epoch
        self.device_epoch = device_epoch
        self.pages: dict[int, UploadPage] = {}
        self.leases: dict[int, InputLease] = {}
        self.pending_submissions: dict[int, PendingSubmission] = {}
        self.next_ticket_id = 1
        self.poll_count = 0
        self.unsafe_reuse_count = 0
        self.post_reset_receipt_commits = 0
        self._last_completed: dict[str, int] = {}
        self.rejections: dict[str, int] = {
            "unknownConsumer": 0,
            "notRequested": 0,
            "leaseIdentityMismatch": 0,
            "generationMismatch": 0,
            "epochMismatch": 0,
            "tokenMismatch": 0,
            "rangeMismatch": 0,
            "pageIdentityMismatch": 0,
            "duplicateSettlement": 0,
            "retireWhileOpen": 0,
            "pageReopenUnsafe": 0,
            "staleTicket": 0,
            "forgedTicket": 0,
            "pendingIndexMismatch": 0,
        }

    @staticmethod
    def _valid_range(offset: int, length: int) -> bool:
        return (
            0 <= offset <= 0xFFFFFFFF
            and 0 < length <= 0xFFFFFFFF
            and offset + length <= 0x100000000
        )

    @staticmethod
    def _single_known_consumer(bit: int) -> bool:
        return bit != 0 and bit & (bit - 1) == 0 and not bit & ~KNOWN_CONSUMERS

    def add_page(self, page_id: int) -> None:
        if page_id <= 0 or page_id in self.pages:
            raise ValueError("page id must be unique and positive")
        self.pages[page_id] = UploadPage(page_id=page_id)

    def add_lease(
        self,
        lease_id: int,
        page_id: int,
        token: int,
        requested_bits: int,
        producer_receipt: FenceReceipt | None,
        *,
        static_range: tuple[int, int] = (0x1000, 0x400),
        palette_range: tuple[int, int] = (0x2000, 0x300),
    ) -> None:
        if lease_id <= 0 or lease_id in self.leases:
            raise ValueError("lease id must be unique and positive")
        if token <= 0:
            raise ValueError("token must be positive")
        if requested_bits == 0 or requested_bits & ~KNOWN_CONSUMERS:
            raise ValueError("requested consumer mask must be known and non-zero")
        if not self._valid_range(*static_range) or not self._valid_range(
            *palette_range
        ):
            raise ValueError("lease ranges must be non-empty uint32 ranges")
        page = self.pages[page_id]
        if page.reusable or page.live_claims and not page.current_lease_ids:
            raise ValueError("reopen a reusable page before adding a new lease")

        lease = InputLease(
            lease_id=lease_id,
            runtime_generation=self.runtime_generation,
            map_epoch=self.map_epoch,
            device_epoch=self.device_epoch,
            token=token,
            page_id=page_id,
            page_generation=page.generation,
            static_byte_offset=static_range[0],
            static_byte_length=static_range[1],
            palette_byte_offset=palette_range[0],
            palette_byte_length=palette_range[1],
            requested_bits=requested_bits,
            producer_receipt=producer_receipt,
        )
        self.leases[lease_id] = lease
        page.current_lease_ids.append(lease_id)
        page.live_claims.add((lease_id, page.generation))
        page.reusable = False

    def access_for(self, lease_id: int, consumer_bit: int) -> LeaseAccess:
        lease = self.leases[lease_id]
        return LeaseAccess(
            lease_id=lease_id,
            consumer_bit=consumer_bit,
            runtime_generation=self.runtime_generation,
            map_epoch=lease.map_epoch,
            device_epoch=lease.device_epoch,
            token=lease.token,
            page_id=lease.page_id,
            page_generation=lease.page_generation,
            static_byte_offset=lease.static_byte_offset,
            static_byte_length=lease.static_byte_length,
            palette_byte_offset=lease.palette_byte_offset,
            palette_byte_length=lease.palette_byte_length,
        )

    def _validate_open_access(self, access: LeaseAccess) -> InputLease | None:
        lease = self.leases.get(access.lease_id)
        if lease is None:
            self.rejections["leaseIdentityMismatch"] += 1
            return None
        if not self._single_known_consumer(access.consumer_bit):
            self.rejections["unknownConsumer"] += 1
            return None
        if not lease.requested_bits & access.consumer_bit:
            self.rejections["notRequested"] += 1
            return None
        if not lease.open_bits & access.consumer_bit:
            self.rejections["duplicateSettlement"] += 1
            return None
        if (
            access.runtime_generation != self.runtime_generation
            or lease.runtime_generation != self.runtime_generation
        ):
            self.rejections["generationMismatch"] += 1
            return None
        if (
            access.map_epoch != lease.map_epoch
            or access.device_epoch != lease.device_epoch
            or access.map_epoch != self.map_epoch
            or access.device_epoch != self.device_epoch
        ):
            self.rejections["epochMismatch"] += 1
            return None
        if access.token != lease.token:
            self.rejections["tokenMismatch"] += 1
            return None
        if (
            access.page_id != lease.page_id
            or access.page_generation != lease.page_generation
        ):
            self.rejections["pageIdentityMismatch"] += 1
            return None
        if (
            access.static_byte_offset != lease.static_byte_offset
            or access.static_byte_length != lease.static_byte_length
            or access.palette_byte_offset != lease.palette_byte_offset
            or access.palette_byte_length != lease.palette_byte_length
        ):
            self.rejections["rangeMismatch"] += 1
            return None
        return lease

    @staticmethod
    def _ticket_from_access(ticket_id: int, access: LeaseAccess) -> SubmissionTicket:
        return SubmissionTicket(
            ticket_id=ticket_id,
            lease_id=access.lease_id,
            consumer_bit=access.consumer_bit,
            runtime_generation=access.runtime_generation,
            map_epoch=access.map_epoch,
            device_epoch=access.device_epoch,
            token=access.token,
            page_id=access.page_id,
            page_generation=access.page_generation,
            static_byte_offset=access.static_byte_offset,
            static_byte_length=access.static_byte_length,
            palette_byte_offset=access.palette_byte_offset,
            palette_byte_length=access.palette_byte_length,
        )

    def begin_gpu_submission(self, access: LeaseAccess) -> SubmissionTicket | None:
        """进入不可逆且等待回执的阶段。"""
        lease = self._validate_open_access(access)
        if lease is None:
            return None
        ticket_id = self.next_ticket_id
        self.next_ticket_id += 1
        ticket = self._ticket_from_access(ticket_id, access)
        self.pending_submissions[ticket_id] = PendingSubmission(ticket=ticket)
        lease.pending_ticket_ids[access.consumer_bit] = ticket_id
        lease.open_bits &= ~access.consumer_bit
        return ticket

    def commit_gpu_submission(
        self, ticket: SubmissionTicket, receipt: FenceReceipt
    ) -> bool:
        """附加回执；重置后原来的精确票据仍然有效。"""
        pending = self.pending_submissions.get(ticket.ticket_id)
        if pending is None:
            self.rejections["staleTicket"] += 1
            return False
        # 对象身份用于模拟不可伪造的能力。即使 dataclass 副本的字段完全相同，
        # 它也不是 begin 返回的那个能力。
        if pending.ticket is not ticket:
            self.rejections["forgedTicket"] += 1
            return False
        lease = self.leases.get(ticket.lease_id)
        if (
            lease is None
            or lease.pending_ticket_ids.get(ticket.consumer_bit)
            != ticket.ticket_id
        ):
            self.rejections["pendingIndexMismatch"] += 1
            return False

        del self.pending_submissions[ticket.ticket_id]
        del lease.pending_ticket_ids[ticket.consumer_bit]
        lease.gpu_receipts[ticket.consumer_bit] = receipt
        if ticket.runtime_generation != self.runtime_generation:
            self.post_reset_receipt_commits += 1
        return True

    def settle_without_gpu(self, access: LeaseAccess) -> bool:
        lease = self._validate_open_access(access)
        if lease is None:
            return False
        lease.no_gpu_bits |= access.consumer_bit
        lease.open_bits &= ~access.consumer_bit
        return True

    def queue_retirement(self, lease_id: int) -> bool:
        lease = self.leases[lease_id]
        if lease.open_bits:
            self.rejections["retireWhileOpen"] += 1
            return False
        # 等待回执的能力可以进入退休流程，但在 pending_bits 归零之前，
        # poll 不得回收该租约。
        lease.retirement_queued = True
        return True

    def reset(
        self,
        *,
        next_map_epoch: int | None = None,
        next_device_epoch: int | None = None,
    ) -> None:
        if next_map_epoch is not None and next_map_epoch <= 0:
            raise ValueError("map epoch must be positive")
        if next_device_epoch is not None and next_device_epoch <= 0:
            raise ValueError("device epoch must be positive")
        self.runtime_generation += 1
        if next_map_epoch is not None:
            self.map_epoch = next_map_epoch
        if next_device_epoch is not None:
            self.device_epoch = next_device_epoch

        for lease in self.leases.values():
            if lease.reclaimed:
                continue
            # 只有尚未开始的消费者可以取消。待定能力会继续存活，
            # 并且之后必须提交其原有的精确回执。
            lease.reset_cancelled_bits |= lease.open_bits
            lease.open_bits = 0
            lease.retirement_queued = True

    @staticmethod
    def _receipt_complete(
        receipt: FenceReceipt | None, completed: Mapping[str, int]
    ) -> bool:
        return receipt is None or completed.get(receipt.fence, 0) >= receipt.value

    def _lease_fences_complete(
        self, lease: InputLease, completed: Mapping[str, int]
    ) -> bool:
        if not self._receipt_complete(lease.producer_receipt, completed):
            return False
        return all(
            self._receipt_complete(receipt, completed)
            for receipt in lease.gpu_receipts.values()
        )

    def _eligible_for_release(self, lease: InputLease) -> bool:
        """分配器状态转换谓词；安全性判定逻辑与其相互独立。"""
        return (
            lease.retirement_queued
            and lease.open_bits == 0
            and not lease.pending_ticket_ids
            and lease.terminal_bits == lease.requested_bits
            and self._lease_fences_complete(lease, self._last_completed)
        )

    def _independent_lease_blockers(self, lease: InputLease) -> list[str]:
        """直接依据原始字段审计账本，不使用 release/reclaimed 状态。"""
        blockers: list[str] = []
        pending_bits = lease.pending_bits
        terminal_bits = lease.terminal_bits
        classified_bits = lease.open_bits | pending_bits | terminal_bits
        overlaps = (
            (lease.open_bits & pending_bits)
            | (lease.open_bits & terminal_bits)
            | (pending_bits & terminal_bits)
        )

        if not lease.retirement_queued:
            blockers.append("retirement-not-queued")
        if classified_bits != lease.requested_bits:
            blockers.append("consumer-classification-mismatch")
        if overlaps:
            blockers.append("consumer-classification-overlap")
        if pending_bits:
            blockers.append("pending-receipt")

        # 校验“租约 -> 全局待定索引”方向，且不把任一侧视为另一侧的权威来源。
        for bit, ticket_id in lease.pending_ticket_ids.items():
            pending = self.pending_submissions.get(ticket_id)
            if pending is None:
                blockers.append(f"ticket-{ticket_id}-missing-global-index")
                continue
            ticket = pending.ticket
            if not pending.irreversible:
                blockers.append(f"ticket-{ticket_id}-not-irreversible")
            expected = (
                lease.lease_id,
                bit,
                lease.runtime_generation,
                lease.map_epoch,
                lease.device_epoch,
                lease.token,
                lease.page_id,
                lease.page_generation,
                lease.static_byte_offset,
                lease.static_byte_length,
                lease.palette_byte_offset,
                lease.palette_byte_length,
            )
            observed = (
                ticket.lease_id,
                ticket.consumer_bit,
                ticket.runtime_generation,
                ticket.map_epoch,
                ticket.device_epoch,
                ticket.token,
                ticket.page_id,
                ticket.page_generation,
                ticket.static_byte_offset,
                ticket.static_byte_length,
                ticket.palette_byte_offset,
                ticket.palette_byte_length,
            )
            if observed != expected:
                blockers.append(f"ticket-{ticket_id}-identity-mismatch")

        # 独立校验“全局待定索引 -> 租约”方向。
        for ticket_id, pending in self.pending_submissions.items():
            ticket = pending.ticket
            if ticket.lease_id != lease.lease_id:
                continue
            if lease.pending_ticket_ids.get(ticket.consumer_bit) != ticket_id:
                blockers.append(f"ticket-{ticket_id}-missing-lease-index")

        if not self._receipt_complete(
            lease.producer_receipt, self._last_completed
        ):
            blockers.append("producer-fence-incomplete")
        for bit, receipt in lease.gpu_receipts.items():
            if not lease.requested_bits & bit or not self._single_known_consumer(bit):
                blockers.append(f"consumer-{bit}-invalid-receipt-bit")
            if not self._receipt_complete(receipt, self._last_completed):
                blockers.append(f"consumer-{bit}-fence-incomplete")
        return blockers

    def _page_blockers(self, page: UploadPage) -> list[str]:
        blockers: list[str] = []
        scanned_ids = {
            lease.lease_id
            for lease in self.leases.values()
            if lease.page_id == page.page_id
            and lease.page_generation == page.generation
        }
        listed_ids = set(page.current_lease_ids)
        if listed_ids != scanned_ids:
            blockers.append("current-lease-index-mismatch")
        for lease_id in sorted(scanned_ids):
            lease = self.leases[lease_id]
            if lease.page_id != page.page_id or lease.page_generation != page.generation:
                blockers.append(f"lease-{lease_id}-page-identity")
                continue
            blockers.extend(
                f"lease-{lease_id}-{reason}"
                for reason in self._independent_lease_blockers(lease)
            )
        for pending in self.pending_submissions.values():
            ticket = pending.ticket
            if ticket.page_id == page.page_id and ticket.page_generation == page.generation:
                blockers.append(f"ticket-{ticket.ticket_id}-pending-index")
        return blockers

    def poll(self, completed: Mapping[str, int]) -> None:
        self.poll_count += 1
        for fence, value in completed.items():
            if value < self._last_completed.get(fence, 0):
                raise ValueError("completed fence values must be monotonic")
            self._last_completed[fence] = value

        for lease in self.leases.values():
            if lease.reclaimed:
                continue
            if self._eligible_for_release(lease):
                lease.reclaimed = True
                page = self.pages[lease.page_id]
                page.live_claims.discard((lease.lease_id, lease.page_generation))

        for page in self.pages.values():
            page.reusable = bool(page.current_lease_ids) and not page.live_claims
        self._assert_no_unsafe_reuse()

    def _assert_no_unsafe_reuse(self) -> None:
        # 此处有意直接审计 open/pending/terminal/fence 状态，
        # 不会把 reclaimed 或空的 live_claims 集合作为证据。
        for page in self.pages.values():
            if not page.reusable:
                continue
            blockers = self._page_blockers(page)
            if blockers:
                self.unsafe_reuse_count += 1
                raise AssertionError(
                    f"unsafe page {page.page_id}:{page.generation}: {blockers}"
                )

    def reopen_page(self, page_id: int) -> bool:
        page = self.pages[page_id]
        blockers = self._page_blockers(page)
        if (
            not page.reusable
            or page.live_claims
            or not page.current_lease_ids
            or blockers
        ):
            self.rejections["pageReopenUnsafe"] += 1
            return False
        page.generation += 1
        page.reuse_count += 1
        page.current_lease_ids.clear()
        page.live_claims.clear()
        page.reusable = False
        return True

    def page_reusable(self, page_id: int) -> bool:
        return self.pages[page_id].reusable

    @staticmethod
    def _receipt_dict(receipt: FenceReceipt | None) -> dict[str, object] | None:
        if receipt is None:
            return None
        return {"fence": receipt.fence, "value": receipt.value}

    @staticmethod
    def _ticket_dict(ticket: SubmissionTicket) -> dict[str, int]:
        return {
            "ticketId": ticket.ticket_id,
            "leaseId": ticket.lease_id,
            "consumerBit": ticket.consumer_bit,
            "runtimeGeneration": ticket.runtime_generation,
            "mapEpoch": ticket.map_epoch,
            "deviceEpoch": ticket.device_epoch,
            "token": ticket.token,
            "pageId": ticket.page_id,
            "pageGeneration": ticket.page_generation,
            "staticByteOffset": ticket.static_byte_offset,
            "staticByteLength": ticket.static_byte_length,
            "paletteByteOffset": ticket.palette_byte_offset,
            "paletteByteLength": ticket.palette_byte_length,
        }

    def snapshot(self) -> dict[str, object]:
        return {
            "runtimeGeneration": self.runtime_generation,
            "mapEpoch": self.map_epoch,
            "deviceEpoch": self.device_epoch,
            "pollCount": self.poll_count,
            "pendingSubmissionCount": len(self.pending_submissions),
            "postResetReceiptCommits": self.post_reset_receipt_commits,
            "unsafeReuseCount": self.unsafe_reuse_count,
            "rejections": dict(self.rejections),
            "pages": {
                str(page_id): {
                    "pageId": page.page_id,
                    "generation": page.generation,
                    "currentLeaseIds": list(page.current_lease_ids),
                    "liveClaims": [
                        list(claim) for claim in sorted(page.live_claims)
                    ],
                    "reusable": page.reusable,
                    "reuseCount": page.reuse_count,
                    "independentBlockers": self._page_blockers(page),
                }
                for page_id, page in sorted(self.pages.items())
            },
            "leases": {
                str(lease_id): {
                    "leaseId": lease.lease_id,
                    "runtimeGeneration": lease.runtime_generation,
                    "mapEpoch": lease.map_epoch,
                    "deviceEpoch": lease.device_epoch,
                    "token": lease.token,
                    "pageId": lease.page_id,
                    "pageGeneration": lease.page_generation,
                    "staticRange": [
                        lease.static_byte_offset,
                        lease.static_byte_length,
                    ],
                    "paletteRange": [
                        lease.palette_byte_offset,
                        lease.palette_byte_length,
                    ],
                    "requestedBits": lease.requested_bits,
                    "openBits": lease.open_bits,
                    "pendingBits": lease.pending_bits,
                    "pendingTicketIds": {
                        str(bit): ticket_id
                        for bit, ticket_id in sorted(
                            lease.pending_ticket_ids.items()
                        )
                    },
                    "gpuBits": lease.gpu_bits,
                    "noGpuBits": lease.no_gpu_bits,
                    "resetCancelledBits": lease.reset_cancelled_bits,
                    "terminalBits": lease.terminal_bits,
                    "retirementQueued": lease.retirement_queued,
                    "reclaimed": lease.reclaimed,
                    "producerReceipt": self._receipt_dict(
                        lease.producer_receipt
                    ),
                    "gpuReceipts": {
                        str(bit): self._receipt_dict(receipt)
                        for bit, receipt in sorted(lease.gpu_receipts.items())
                    },
                    "independentReleaseBlockers": (
                        self._independent_lease_blockers(lease)
                    ),
                }
                for lease_id, lease in sorted(self.leases.items())
            },
            "pendingSubmissions": {
                str(ticket_id): self._ticket_dict(pending.ticket)
                for ticket_id, pending in sorted(self.pending_submissions.items())
            },
        }


def parse_execution_route(value: str | None) -> dict[str, object]:
    normalized = "" if value is None else value.lower()
    explicit = bool(normalized)
    if normalized in ("", "compute", "cs", "0"):
        return {"route": ROUTE_COMPUTE, "explicit": explicit, "invalid": False}
    if normalized in ("vertex_shader", "vertex", "vs", "1"):
        return {
            "route": ROUTE_VERTEX_SHADER,
            "explicit": True,
            "invalid": False,
        }
    if normalized in ("vertex_shader_input_only", "vs_input_only", "2"):
        return {
            "route": ROUTE_VERTEX_SHADER_INPUT_ONLY,
            "explicit": True,
            "invalid": False,
        }
    if normalized in ("vertex_shader_bypass", "vs_bypass", "3"):
        return {
            "route": ROUTE_VERTEX_SHADER_BYPASS,
            "explicit": True,
            "invalid": False,
        }
    return {"route": ROUTE_COMPUTE, "explicit": True, "invalid": True}


def deterministic_cases() -> list[dict[str, object]]:
    cases: list[dict[str, object]] = []

    # 即使生产者已经完成，且消费者栅栏名称对应的值已任意推进，
    # 仍不能释放回执能力尚处于待定状态的提交。
    model = InputLeaseModel()
    model.add_page(1)
    model.add_lease(1, 1, 101, MAIN, FenceReceipt("producer", 2))
    ticket = model.begin_gpu_submission(model.access_for(1, MAIN))
    assert ticket is not None and model.queue_retirement(1)
    model.poll({"producer": 2, "main": 999})
    pending_receipt_blocked = not model.page_reusable(1)
    assert pending_receipt_blocked and len(model.pending_submissions) == 1
    assert model.commit_gpu_submission(ticket, FenceReceipt("main", 1000))
    model.poll({"producer": 2, "main": 999})
    committed_fence_blocked = not model.page_reusable(1)
    assert committed_fence_blocked
    model.poll({"producer": 2, "main": 1000})
    assert model.page_reusable(1)
    cases.append({
        "name": "pending_receipt_then_consumer_fence_gate",
        "pendingReceiptBlockedReuse": pending_receipt_blocked,
        "committedFenceBlockedReuse": committed_fence_blocked,
        "snapshot": model.snapshot(),
    })

    # Outline 不执行 GPU 读取，而 Main 与 Shadow 各自保留独立提交的回执。
    model = InputLeaseModel()
    model.add_page(2)
    model.add_lease(
        2, 2, 102, MAIN | SHADOW | OUTLINE, FenceReceipt("producer", 2)
    )
    main_ticket = model.begin_gpu_submission(model.access_for(2, MAIN))
    shadow_ticket = model.begin_gpu_submission(model.access_for(2, SHADOW))
    assert main_ticket is not None and shadow_ticket is not None
    assert model.commit_gpu_submission(main_ticket, FenceReceipt("frame", 5))
    assert model.commit_gpu_submission(shadow_ticket, FenceReceipt("frame", 11))
    assert model.settle_without_gpu(model.access_for(2, OUTLINE))
    assert model.queue_retirement(2)
    model.poll({"producer": 2, "frame": 10})
    assert not model.page_reusable(2)
    model.poll({"producer": 2, "frame": 11})
    assert model.page_reusable(2)
    cases.append({
        "name": "mixed_gpu_and_no_gpu_consumers",
        "snapshot": model.snapshot(),
    })

    # 在操作进入不可逆阶段之前，准入检查会拒绝所有发生变化的身份维度。
    # 即使真实票据副本的字段完全相同，它也不是 begin 返回的那个能力。
    model = InputLeaseModel()
    model.add_page(3)
    model.add_lease(3, 3, 103, MAIN, None)
    access = model.access_for(3, MAIN)
    assert model.begin_gpu_submission(
        replace(access, runtime_generation=access.runtime_generation + 1)
    ) is None
    assert model.begin_gpu_submission(
        replace(access, map_epoch=access.map_epoch + 1)
    ) is None
    assert model.begin_gpu_submission(replace(access, token=access.token + 1)) is None
    assert model.begin_gpu_submission(
        replace(access, palette_byte_length=access.palette_byte_length + 4)
    ) is None
    assert model.begin_gpu_submission(
        replace(access, page_generation=access.page_generation + 1)
    ) is None
    assert model.begin_gpu_submission(replace(access, consumer_bit=0x20)) is None
    assert model.begin_gpu_submission(replace(access, consumer_bit=SHADOW)) is None
    ticket = model.begin_gpu_submission(access)
    assert ticket is not None
    forged_ticket = replace(ticket)
    assert forged_ticket == ticket and forged_ticket is not ticket
    assert not model.commit_gpu_submission(forged_ticket, FenceReceipt("bad", 1))
    assert len(model.pending_submissions) == 1
    assert model.queue_retirement(3)
    model.poll({"main": 100})
    assert not model.page_reusable(3)
    assert model.commit_gpu_submission(ticket, FenceReceipt("main", 101))
    assert not model.commit_gpu_submission(ticket, FenceReceipt("main", 101))
    model.poll({"main": 101})
    assert model.page_reusable(3)
    expected_rejections = {
        "generationMismatch": 1,
        "epochMismatch": 1,
        "tokenMismatch": 1,
        "rangeMismatch": 1,
        "pageIdentityMismatch": 1,
        "unknownConsumer": 1,
        "notRequested": 1,
        "forgedTicket": 1,
        "staleTicket": 1,
    }
    for name, expected in expected_rejections.items():
        assert model.rejections[name] == expected
    cases.append({
        "name": "identity_and_ticket_capability_rejects",
        "expectedRejections": expected_rejections,
        "snapshot": model.snapshot(),
    })

    # 重置会改变当前 epoch，并且只取消从未开始的消费者。
    # 原来的精确 Main 票据在重置后仍可提交其回执。
    model = InputLeaseModel()
    model.add_page(4)
    model.add_lease(
        4, 4, 104, MAIN | SHADOW | OUTLINE, FenceReceipt("producer", 2)
    )
    old_ticket = model.begin_gpu_submission(model.access_for(4, MAIN))
    assert old_ticket is not None
    model.reset(next_map_epoch=2, next_device_epoch=3)
    lease = model.leases[4]
    assert lease.pending_bits == MAIN
    assert lease.reset_cancelled_bits == SHADOW | OUTLINE
    model.poll({"producer": 2, "old-main": 999})
    reset_pending_blocked = not model.page_reusable(4)
    assert reset_pending_blocked
    assert model.commit_gpu_submission(old_ticket, FenceReceipt("old-main", 1000))
    assert model.post_reset_receipt_commits == 1
    model.poll({"producer": 2, "old-main": 999})
    assert not model.page_reusable(4)
    model.poll({"producer": 2, "old-main": 1000})
    assert model.page_reusable(4)
    cases.append({
        "name": "reset_retains_pending_ticket_and_old_receipt",
        "pendingReceiptPreserved": reset_pending_blocked,
        "oldTicketCommittedAfterReset": True,
        "snapshot": model.snapshot(),
    })

    # 只要当前页 generation 中仍有任一子租约持有字节，物理页就不能重新开放。
    model = InputLeaseModel()
    model.add_page(5)
    producer = FenceReceipt("producer", 3)
    model.add_lease(5, 5, 105, MAIN, producer)
    model.add_lease(6, 5, 106, SHADOW, producer)
    first_ticket = model.begin_gpu_submission(model.access_for(5, MAIN))
    second_ticket = model.begin_gpu_submission(model.access_for(6, SHADOW))
    assert first_ticket is not None and second_ticket is not None
    assert model.commit_gpu_submission(first_ticket, FenceReceipt("frame", 5))
    assert model.commit_gpu_submission(second_ticket, FenceReceipt("frame", 12))
    assert model.queue_retirement(5) and model.queue_retirement(6)
    model.poll({"producer": 3, "frame": 5})
    assert model.leases[5].reclaimed
    shared_page_blocked = not model.page_reusable(5)
    assert shared_page_blocked and not model.reopen_page(5)
    model.poll({"producer": 3, "frame": 12})
    assert model.page_reusable(5)
    cases.append({
        "name": "shared_page_waits_for_all_leases",
        "sharedPageReuseBlocked": shared_page_blocked,
        "snapshot": model.snapshot(),
    })

    # 从未启动 GPU 工作的租约不存在待定能力，也不需要伪造消费者栅栏。
    model = InputLeaseModel()
    model.add_page(6)
    model.add_lease(7, 6, 107, MAIN | OUTLINE, None)
    assert model.settle_without_gpu(model.access_for(7, MAIN))
    assert model.settle_without_gpu(model.access_for(7, OUTLINE))
    assert model.queue_retirement(7)
    model.poll({})
    assert model.page_reusable(6)
    cases.append({
        "name": "no_gpu_settlement_needs_no_fake_fence",
        "snapshot": model.snapshot(),
    })

    # 演练真实的页面 generation 复用。即使新租约使用相同的物理页、token 与字节范围，
    # 原来的精确票据也不能触及它；新票据的副本同样无法提交。
    model = InputLeaseModel(map_epoch=10, device_epoch=20)
    model.add_page(7)
    model.add_lease(8, 7, 777, MAIN, FenceReceipt("producer", 1))
    stale_ticket = model.begin_gpu_submission(model.access_for(8, MAIN))
    assert stale_ticket is not None and model.queue_retirement(8)
    assert not model.reopen_page(7)
    model.poll({"producer": 1, "old": 99})
    assert not model.page_reusable(7)
    assert model.commit_gpu_submission(stale_ticket, FenceReceipt("old", 100))
    model.poll({"producer": 1, "old": 100})
    assert model.page_reusable(7) and model.reopen_page(7)
    assert model.pages[7].generation == 2

    model.add_lease(9, 7, 777, MAIN, FenceReceipt("producer", 1))
    assert not model.commit_gpu_submission(stale_ticket, FenceReceipt("old", 100))
    assert model.leases[9].open_bits == MAIN
    new_ticket = model.begin_gpu_submission(model.access_for(9, MAIN))
    assert new_ticket is not None
    copied_new_ticket = replace(new_ticket)
    assert not model.commit_gpu_submission(
        copied_new_ticket, FenceReceipt("new", 200)
    )
    assert model.leases[9].pending_bits == MAIN
    model.reset(next_map_epoch=11, next_device_epoch=21)
    model.poll({"producer": 1, "new": 199})
    assert not model.page_reusable(7)
    assert model.commit_gpu_submission(new_ticket, FenceReceipt("new", 200))
    model.poll({"producer": 1, "new": 199})
    assert not model.page_reusable(7)
    model.poll({"producer": 1, "new": 200})
    assert model.page_reusable(7) and model.reopen_page(7)
    assert model.pages[7].generation == 3
    cases.append({
        "name": "page_reopen_generation_and_stale_ticket_aba",
        "finalPageGeneration": model.pages[7].generation,
        "reuseCount": model.pages[7].reuse_count,
        "staleTicketRejectedAfterReuse": True,
        "copiedTicketRejected": True,
        "snapshot": model.snapshot(),
    })

    return cases


def _ordered(text: str, needles: tuple[str, ...]) -> bool:
    cursor = 0
    for needle in needles:
        cursor = text.find(needle, cursor)
        if cursor < 0:
            return False
        cursor += len(needle)
    return True


def audit_runtime_integration(repo_root: Path) -> dict[str, object]:
    """只读核对 VS-A/VS-B0/VS-B1 运行时代码是否具备最窄安全接线。"""
    paths = {
        "device": repo_root / "src/d3d9/d3d9_device.cpp",
        "deviceHeader": repo_root / "src/d3d9/d3d9_device.h",
        "manager": repo_root / "src/d3d9/war3/gpu_skin/war3_gpu_skin_manager.cpp",
        "managerHeader": repo_root / "src/d3d9/war3/gpu_skin/war3_gpu_skin_manager.h",
        "resources": repo_root / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.cpp",
        "resourceHeader": repo_root / "src/d3d9/war3/gpu_skin/war3_gpu_skin_resources.h",
        "types": repo_root / "src/d3d9/war3/gpu_skin/war3_gpu_skin_types.h",
        "fixed": repo_root / "src/d3d9/d3d9_fixed_function.cpp",
        "shader": repo_root / "src/d3d9/shaders/d3d9_fixed_function_vert.vert",
        "shadow": repo_root / "src/d3d9/d3d9_war3_shadow.cpp",
        "scene": repo_root / "src/d3d9/d3d9_war3_scene.h",
        "shadowShader": (
            repo_root / "subprojects/war3fx/shaders/war3_shadow_caster_vert.vert"
        ),
        "meson": repo_root / "src/d3d9/meson.build",
    }
    text = {
        name: path.read_text(encoding="utf-8")
        for name, path in paths.items()
    }
    draw_block_start = text["device"].find(
        "cGpuSkinVsMainOverride = gpuSkinVsMainOverride"
    )
    draw_block = (
        text["device"][draw_block_start:]
        if draw_block_start >= 0 else ""
    )
    checks = {
        "defaultRouteCompute": (
            "GpuSkinExecutionRoute executionRoute = "
            "GpuSkinExecutionRoute::Compute" in text["types"]
        ),
        "explicitRouteRequired": (
            "m_executionRouteExplicit && !m_executionRouteInvalid"
            in text["manager"]
            and "executionRouteExplicit()" in text["device"]
        ),
        "paletteCopiedToDeviceLocalLease": all(token in text["manager"] for token in (
            "GpuSkinInputCopy copy = {}",
            "copy.destination = storage.slice",
            "batch.inputStorageLeases.push_back(storage)",
        )),
        "runtimeReceiptCapabilityCreated": all(
            token in text["manager"] for token in (
                "std::make_shared<GpuSkinInputLeaseReceipt>()",
                "input.storagePageGeneration = storage.pageGeneration",
                "input.receipt = receipt",
                "batch.inputReceipts.push_back(std::move(receipt))",
            )
        ),
        "runtimeReceiptIdentityValidated": all(
            token in text["manager"] for token in (
                "inputReceiptMatchesStorage",
                "receipt->storagePageGeneration == storage.pageGeneration",
                "SameGpuSkinInputLeaseDesc((*receipt)->desc, input.desc)",
            )
        ),
        "runtimeReceiptEpochPropagated": all(
            token in text["manager"] for token in (
                "inputLease.receipt->desc.dispatchEpoch",
                "inputLease.receipt->desc.uploadEpoch",
            )
        ),
        "inputStorageRetiredWithConsumerFence": (
            "retireLeases(submitted->second.inputStorageLeases)"
            in text["manager"]
            or all(token in text["manager"] for token in (
                "GpuSkinInputLeaseReceiptState::ConsumerCommitted",
                "resources->retireOutput(*storage, fence, value)",
                "settleInputReceipt(",
            ))
        ),
        "producerOnlyAndCancelTerminalStates": all(
            token in text["manager"] for token in (
                "GpuSkinInputLeaseReceiptState::ProducerOnly",
                "GpuSkinInputLeaseReceiptState::Cancelled",
                "cancelInputReceipts",
            )
        ),
        "consumerFenceIdentityStored": all(
            token in text["manager"] for token in (
                "receipt->consumerFence = fence",
                "receipt->consumerFenceValue = value",
            )
        ),
        "nativeEpochPropagatedToInputLease": all(
            token in text["manager"] for token in (
                "inputLease.desc.dispatchEpoch",
                "inputLease.desc.uploadEpoch",
            )
        ),
        "copyBarrierFeedsVertexShader": all(token in text["device"] for token in (
            "for (const GpuSkinInputCopy& copy : inputCopies)",
            "VK_PIPELINE_STAGE_TRANSFER_BIT",
            "VK_PIPELINE_STAGE_VERTEX_SHADER_BIT",
            "VK_ACCESS_TRANSFER_WRITE_BIT",
            "VK_ACCESS_SHADER_READ_BIT",
        )),
        "outputPageSupportsPaletteStorage": all(
            token in text["resources"] for token in (
                "VK_BUFFER_USAGE_TRANSFER_DST_BIT",
                "VK_PIPELINE_STAGE_VERTEX_SHADER_BIT",
                "VK_ACCESS_SHADER_READ_BIT",
                "VK_ACCESS_TRANSFER_WRITE_BIT",
            )
        ),
        "mainExactGatePresent": all(token in text["device"] for token in (
            "gpuSkinVsInputExact",
            "gpuSkinVsMainOverride",
            "gpuSkinInputLease.storagePageGeneration != 0u",
            "GetFVF() == 0x112u",
            "GetStreamMask() == 1u",
            "D3DRS_INDEXEDVERTEXBLENDENABLE",
        )),
        "bindDrawClearUnbindRestoreOrdered": _ordered(draw_block, (
            "std::move(cGpuSkinVsShader)",
            "VK_SHADER_STAGE_VERTEX_BIT, 64u, std::move(staticView)",
            "VK_SHADER_STAGE_VERTEX_BIT, 65u, std::move(paletteView)",
            "sizeof(cGpuSkinVsDrawParams)",
            "ctx->drawIndexed(1u, &draw)",
            "clearedParams",
            "VK_SHADER_STAGE_VERTEX_BIT, 64u, nullptr",
            "VK_SHADER_STAGE_VERTEX_BIT, 65u, nullptr",
            "std::move(cGpuSkinStockVsShader)",
        )),
        "privateShaderVariantPresent": all(token in text["shader"] for token in (
            "D3D9_WAR3_GPU_SKIN",
            "binding = 64",
            "binding = 65",
            "tryLoadWar3GpuSkinVertex",
        )),
        "localPushContractExact": all(token in text["fixed"] for token in (
            "DxvkPushDataBlock(VK_SHADER_STAGE_VERTEX_BIT, 64u, 32u",
            '"War3 GPU Skin FF VS"',
        )),
        "privateShaderBuildTargetPresent": all(token in text["meson"] for token in (
            "d3d9_war3_gpu_skin_ff_vs",
            "-DD3D9_WAR3_GPU_SKIN=1",
            "d3d9_war3_gpu_skin_fixed_function_vert.h",
        )),
        "runtimeCountersPresent": all(token in text["device"] for token in (
            "diag vsRoute",
            "m_war3GpuSkinVsMainDrawsSubmitted",
            "m_war3GpuSkinVsMainBindingsCleared",
        )),
        "inputOnlyRouteExplicitlyParsed": all(
            token in text["resources"] + text["types"]
            for token in (
                "VertexShaderInputOnly",
                '"vertex_shader_input_only"',
                '"vs_input_only"',
            )
        ),
        "inputOnlySkipsComputeOutputAndJob": all(
            token in text["manager"] + text["resources"]
            for token in (
                "finalizeInputOnlyCandidate",
                "allocatePaletteUpload",
                "vsInputOnlyComputeJobsSkipped",
                "vsInputOnlyOutputBytesSkipped",
            )
        ),
        "inputOnlyP4AuthorityStaysCold": all(
            token in text["manager"] for token in (
                "IsVertexShaderInputOnlyRoute(m_executionRoute)",
                "GpuSkinManagerFallbackReason::BypassHostRejected",
            )
        ),
        "inputOnlyMainShadowLeaseExact": all(
            token in text["device"] + text["manager"] for token in (
                "gpuSkinInputOnlyShadowCandidate",
                "gpuSkinVsExpectedConsumers",
                "GpuSkinConsumerBits::Main",
                "GpuSkinConsumerBits::Shadow",
                "entry.gpuSkinInput = gpuSkinSemanticInput",
            )
        ),
        "inputOnlyFallbackBoundaryExact": all(
            token in text["manager"] for token in (
                "candidate.bypassOpaqueEligible",
                "candidate.outputFormat == 2u",
                "candidate.sourceUvLayerCount == 1u",
                "finalizeInputOnlyCandidate(",
                "recordFallback(GpuSkinManagerFallbackReason::LayoutChanged)",
            )
        ),
        "inputOnlyCpuFallbackRequiresColdP4": all(
            token in text["manager"] for token in (
                "const bool inputOnlyCpuFallback",
                "!prepared->bypassCommitted",
                "failure == GpuSkinConsumerFailure::CpuFallback",
                "!inputOnlyCpuFallback",
            )
        ),
        "bypassRouteExplicitlyParsed": all(
            token in text["resources"] + text["types"]
            for token in (
                "VertexShaderBypass",
                '"vertex_shader_bypass"',
                '"vs_bypass"',
            )
        ),
        "bypassUsesExactInputCapability": all(
            token in text["manager"] + text["device"]
            for token in (
                "isExactBypassInputLease",
                "hostRequest.inputLease",
                "vsBypassInputExact",
                "routeCapabilityExact",
                "exactVsBypassCapability",
                "output.desc.consumerBits & ~expectedConsumers",
            )
        ),
        "bypassMainStateClosedBeforeKernel": all(
            token in text["device"] for token in (
                "vsBypassMainStateExact",
                "War3ShouldOverrideWorldMaterial",
                "War3ShouldOverridePostProcessMaterial",
                "GetFVF() == 0x112u",
                "War3ShouldDrawOutline",
                "if (vsBypassRoute && outlineRequested)",
            )
        ),
        "bypassShadowProducerClosedBeforeKernel": all(
            token in text["device"] for token in (
                "vsBypassShadowConsumerReady",
                "War3SemanticConsumerEnabled()",
                "IsSemanticSceneSubmissionRuntimeEnabled()",
                "IsSemanticSceneDisableLegacyShadowCaptureRuntimeEnabled()",
                "if (!vsBypassShadowConsumerReady)",
            )
        ),
        "bypassMainShaderCannotReadPoisonFallback": all(
            token in text["types"] + text["shader"] for token in (
                "kGpuSkinVsDrawBypassActiveMagic",
                "War3GpuSkinBypassActiveMagic",
                "!war3GpuSkinActive",
                "gl_Position = vec4(2.0, 2.0, 2.0, 1.0)",
            )
        ),
        "bypassShadowUsesStaticPositionAndUv": all(
            token in text["device"] + text["scene"] for token in (
                "gpuSkinSemanticDirectOnly",
                "entry.positionBuffer = gpuSkinSemanticInput.staticSource.buffer()",
                "directLayout.texcoord0Offset",
                "bool irreversible = false",
            )
        ),
        "bypassShadowCannotReadPoisonFallback": all(
            token in text["shadow"] + text["shadowShader"] for token in (
                "kShadowCasterFlagGpuSkinNoFallback",
                "directOnlyStateExact",
                "kGpuSkinNoFallbackFlag",
                "!gpuSkinDirectLoaded",
            )
        ),
        "staticAtlasSupportsDirectVertexBinding": all(
            token in text["resources"] for token in (
                "VK_BUFFER_USAGE_VERTEX_BUFFER_BIT",
                "VK_PIPELINE_STAGE_VERTEX_INPUT_BIT",
                "VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT",
            )
        ),
    }
    return {
        "present": all(checks.values()),
        "checks": checks,
        "files": {name: str(path) for name, path in paths.items()},
        "proofBoundary": "静态源码闭合，不替代构建、运行、画面或性能证明。",
    }


def build_payload(repo_root: Path | None = None) -> dict[str, object]:
    # 针对 C++ 聚合体脚手架执行独立的原生布局算术校验。
    assert struct.calcsize("<8I") == 32
    assert struct.calcsize("<6Q10I") == 88

    compute_default = {
        "route": ROUTE_COMPUTE,
        "explicit": False,
        "invalid": False,
    }
    compute_explicit = {
        "route": ROUTE_COMPUTE,
        "explicit": True,
        "invalid": False,
    }
    vertex_explicit = {
        "route": ROUTE_VERTEX_SHADER,
        "explicit": True,
        "invalid": False,
    }
    input_only_explicit = {
        "route": ROUTE_VERTEX_SHADER_INPUT_ONLY,
        "explicit": True,
        "invalid": False,
    }
    bypass_explicit = {
        "route": ROUTE_VERTEX_SHADER_BYPASS,
        "explicit": True,
        "invalid": False,
    }
    invalid_explicit = {
        "route": ROUTE_COMPUTE,
        "explicit": True,
        "invalid": True,
    }
    route_inputs = {
        "unset": None,
        "empty": "",
        "compute": "compute",
        "computeUpper": "CoMpUtE",
        "csAlias": "cs",
        "zeroAlias": "0",
        "vertexShader": "vertex_shader",
        "vertexAlias": "vertex",
        "vsAlias": "vs",
        "oneAlias": "1",
        "vertexInputOnly": "vertex_shader_input_only",
        "vertexInputOnlyAlias": "vs_input_only",
        "twoAlias": "2",
        "vertexBypass": "vertex_shader_bypass",
        "vertexBypassAlias": "vs_bypass",
        "threeAlias": "3",
        "unknown": "future_magic",
    }
    route_cases = {
        name: parse_execution_route(value)
        for name, value in route_inputs.items()
    }
    route_expected = {
        "unset": compute_default,
        "empty": compute_default,
        "compute": compute_explicit,
        "computeUpper": compute_explicit,
        "csAlias": compute_explicit,
        "zeroAlias": compute_explicit,
        "vertexShader": vertex_explicit,
        "vertexAlias": vertex_explicit,
        "vsAlias": vertex_explicit,
        "oneAlias": vertex_explicit,
        "vertexInputOnly": input_only_explicit,
        "vertexInputOnlyAlias": input_only_explicit,
        "twoAlias": input_only_explicit,
        "vertexBypass": bypass_explicit,
        "vertexBypassAlias": bypass_explicit,
        "threeAlias": bypass_explicit,
        "unknown": invalid_explicit,
    }
    assert route_cases == route_expected

    cases = deterministic_cases()
    unsafe_reuses = sum(
        int(case["snapshot"]["unsafeReuseCount"]) for case in cases
    )
    pending_at_end = sum(
        int(case["snapshot"]["pendingSubmissionCount"]) for case in cases
    )
    assert unsafe_reuses == 0
    assert pending_at_end == 0
    resolved_root = repo_root or Path(__file__).resolve().parents[1]
    runtime_audit = audit_runtime_integration(resolved_root)
    assert runtime_audit["present"] is True

    return {
        "schemaVersion": 6,
        "positioning": {
            "offlineExecutableContract": True,
            "runtimeProof": False,
            "runtimeImplementationPresent": True,
            "claim": "离线状态机与源码静态接线均闭合；仍需独立构建和隔离运行硬门。",
        },
        "scope": {
            "offlineOnly": True,
            "runtimeActivationDefault": False,
            "fixedFunctionShaderModified": True,
            "p4AuthorizationModified": True,
            "buildPerformed": False,
            "deployPerformed": False,
            "war3Launched": False,
        },
        "abi": {
            "gpuSkinVsDrawParamsBytes": struct.calcsize("<8I"),
            "gpuSkinVsDrawParamsFormat": "8 little-endian uint32",
            "gpuSkinInputLeaseDescBytes": struct.calcsize("<6Q10I"),
            "gpuSkinInputLeaseDescFormat": (
                "6 little-endian uint64 + 10 little-endian uint32"
            ),
        },
        "executionRoute": {
            "default": ROUTE_COMPUTE,
            "unknownPolicy": "compute plus explicit=true invalid=true",
            "cases": route_cases,
        },
        "runtimeIntegrationAudit": runtime_audit,
        "runtimePaletteStrategy": {
            "kind": "device-local-copy",
            "directProducerUploadPageRead": False,
            "retirementAuthority": "现有 output lease 的帧尾 consumer fence",
            "computeOutputRetainedAsFallback": True,
            "inputOnlyComputeOutputRetained": False,
            "inputOnlyCpuKernelRetained": True,
            "inputOnlyP4BypassAuthority": 0,
            "vsB1ComputeOutputRetained": False,
            "vsB1CpuKernelRetainedAfterExactPreflight": False,
            "vsB1ShadowCpuPositionFallbackAllowed": False,
            "vsB1ShadowRequiresSemanticProducerBeforeKernel": True,
            "vsB1DefaultEnabled": False,
        },
        "contractBoundary": {
            "beginGpuSubmissionIsIrreversible": True,
            "beginCreatesPendingReceiptCapability": True,
            "commitRequiresExactTicketObject": True,
            "oldExactPendingTicketSurvivesReset": True,
            "resetCancelsOnlyNotBegunOpenBits": True,
            "pendingMustBeZeroBeforeReclaim": True,
            "producerFenceAloneAuthorizesReuse": False,
            "consumerGpuSubmissionRequiresCommittedFence": True,
            "noGpuSettlementRequiresSyntheticFence": False,
            "sharedPageRequiresEveryLeaseReleased": True,
            "pageReuseAdvancesGeneration": True,
            "drawActivationAuthority": 0,
            "p4BypassAuthority": 0,
            "modelDoesNotGrantRuntimeAuthority": True,
            "vsB1RuntimeAuthorityRequiresHostExactPreflight": True,
        },
        "summary": {
            "deterministicCases": len(cases),
            "unsafePageReuses": unsafe_reuses,
            "pendingSubmissionsAtCaseEnd": pending_at_end,
            "pendingReceiptReuseBlocked": True,
            "postResetOldTicketReceiptAccepted": True,
            "staleTicketAbaRejected": True,
            "realPageReopenGenerationsExercised": 2,
        },
        "cases": cases,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifact-root", type=Path, default=Path("AutoTest/artifacts")
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument("--stdout", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    payload = build_payload()
    encoded = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if args.self_test and args.output is None and not args.stdout:
        print("gpu-skin VS input-lease offline self-test: PASS")
        return 0

    output = args.output
    if output is None:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output = (
            args.artifact_root
            / f"gpu_skin_vs_input_lease_offline_{stamp}"
            / "result.json"
        )
        output.parent.mkdir(parents=True, exist_ok=False)
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(encoded, encoding="utf-8")
    print(output.parent)
    if args.stdout:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
