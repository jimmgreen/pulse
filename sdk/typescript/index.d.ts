/** Public types for the Pulse development-preview contract. */
export * from "./manifest.generated";
export * from "./api.generated";

/** Serialize the manifest as JSON text; a filesystem path is not accepted. */
export interface ValidateExtensionManifestArgs {
    manifest: string;
}

/** Normal tool result. Invalid tool arguments instead produce invalid_args. */
export type ExtensionManifestValidationResult =
    | { valid: true; error: "" }
    | { valid: false; error: string };

/** Read-only template retrieval; discover supported IDs from the host first. */
export interface GetExtensionTemplateArgs { template: string; }
