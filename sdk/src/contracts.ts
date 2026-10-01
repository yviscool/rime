import type { ActionV1Identity, ActionResultV1 } from "../../contracts/generated/contracts";

export * from "../../contracts/generated/contracts";

/** Compatibility aliases for existing SDK consumers. */
export type ActionIdentity = ActionV1Identity;
export type ActionStatus = ActionResultV1["status"];
