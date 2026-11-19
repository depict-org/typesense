import path from "path";
import type { TypesenseProcessManager } from "@/services/typesense-process";
import type { ErrorWithMessage } from "@/utils/error";
import type { IDockerComposeResult } from "docker-compose";
import type { Ora } from "ora";
import type { CollectionCreateSchema } from "typesense/lib/Typesense/Collections";

import { run } from "docker-compose";
import { errAsync, okAsync, ResultAsync } from "neverthrow";

import { toErrorWithMessage } from "@/utils/error";
import { logger, LogLevel } from "@/utils/logger";
import { findRoot } from "@/utils/package-info";

interface BaseK6Env {
  API_KEY: string;
  PORT: number;
  COLLECTION_NAME: string;
  HOST: string;
  COMMIT_HASH: string;
}

export interface IndexK6Env extends BaseK6Env {
  BATCH_SIZE: number;
}

export interface SearchK6Env extends BaseK6Env {
  DURATION: string;
}

export type K6Env = IndexK6Env & SearchK6Env;

interface LoadTestConfig {
  batchSize: number;
  duration: string;
  apiKey: string;
  port: number;
  commitHash: string;
  typesenseProcessManager: TypesenseProcessManager;
  spinner: Ora;
}

export class K6Benchmarks {
  private readonly config: LoadTestConfig;
  private readonly isInCi: boolean;
  public static readonly COLLECTION_NAME = "songs";
  public static readonly DATASET_URL = "https://dl.typesense.org/datasets/musicbrainz-1M-songs.jsonl.tar.gz";
  public static readonly COLLECTION_SCHEMA: CollectionCreateSchema = {
    name: K6Benchmarks.COLLECTION_NAME,
    fields: [
      { name: "album_name", type: "string" },
      { name: "country", type: "string", facet: true },
      { name: "genres", type: "string[]", facet: true },
      { name: "primary_artist_name", type: "string", facet: true },
      { name: "release_date", type: "int64" },
      { name: "release_decade", type: "string", facet: true },
      { name: "release_group_types", type: "string[]", facet: true },
      { name: "title", type: "string" },
      { name: "track_id", type: "string" },
      { name: "urls", type: "object[]", optional: true },
    ],
    enable_nested_fields: true,
  };
  public static readonly REQUIRED_SERVICES = ["grafana", "influxdb"];

  constructor(config: LoadTestConfig) {
    this.config = config;
    this.isInCi = Boolean(process.env.CI) || false;
  }

  public performSearchBenchmark(): ResultAsync<void, ErrorWithMessage> {
    return this.getSearchBenchmarkPath()
      .andThen((path) => this.executeK6Benchmark({ name: "search", scriptPath: path }))
      .map(() => {
        this.config.spinner.succeed("Search benchmark complete");
      });
  }

  public performIndexingBenchmark(): ResultAsync<void, ErrorWithMessage> {
    return this.getIndexingBenchmarkPath().andThen((path) => {
      return this.createBenchmarkCollection()
        .andThen(() => this.executeK6Benchmark({ name: "indexing", scriptPath: path }))
        .map(() => {
          this.config.spinner.succeed("Indexing benchmark complete");
        });
    });
  }

  private executeK6Benchmark(options: {
    scriptPath: string;
    name: string;
    additionalVars?: Record<string, unknown>;
  }): ResultAsync<void, ErrorWithMessage> {
    const { scriptPath, name } = options;
    const envVarString = this.buildK6EnvironmentVars(options.additionalVars);
    this.config.spinner.start(`Running ${name} benchmark\n`);

    const command = [
      "run",
      envVarString,
      scriptPath,
      logger.getLevel() <= LogLevel.DEBUG ? "--quiet" : "",
    ].join(" ");

    const errors: string[] = [];
    const warnings: string[] = [];

    return ResultAsync.fromPromise(
      run("k6", command, {
        commandOptions: ["--no-deps", "--remove-orphans"],
        log: logger.getLevel() === LogLevel.DEBUG,
        cwd: findRoot(process.cwd()),
        callback: (chunk) => this.processLogChunk(chunk, errors, warnings),
      }),
      toErrorWithMessage,
    ).andThen((result) => this.handleK6Result(result, errors, warnings));
  }

  private processLogChunk(chunk: Buffer, errors: string[], warnings: string[]): void {
    const log = chunk.toString();
    if (log.includes("level=error")) {
      errors.push(log.trim());
    }
    if (log.includes("level=warn")) {
      warnings.push(log.trim());
    }
  }

  private handleK6Result(
    result: IDockerComposeResult,
    errors: string[],
    warnings: string[],
  ): ResultAsync<void, ErrorWithMessage> {
    // First trim any leading/trailing whitespace
    const cleanOutput = result.out.trim();

    const checksLine = cleanOutput.split("\n").find((line) => line.trim().startsWith("checks_succeeded"));

    if (!checksLine) {
      // Fallback to the old format that starts with "checks" if checks_succeeded is not found
      const oldFormatLine = cleanOutput.split("\n").find((line) => line.trim().startsWith("checks"));

      if (!oldFormatLine) {
        return errAsync({
          message: "Could not find checks line in output",
        });
      }

      // Extract the pass rate from the old format line
      const checkMatch = /([0-9.]+)%/.exec(oldFormatLine);
      const checksPassRate = parseFloat(checkMatch?.[1] ?? "0");

      this.config.spinner.stop();
      logger.info(`Checks pass rate: ${checksPassRate}%`);

      if (errors.length > 0) {
        logger.error(`Errors: \n\n${errors.join("\n")}`);
      }
      if (warnings.length > 0) {
        logger.warn(`Warnings: \n\n${warnings.join("\n")}`);
      }

      if (checksPassRate < 97) {
        return errAsync({
          message: `k6 tests failed - ${checksPassRate}% checks passed`,
        });
      }

      this.config.spinner.succeed("Benchmark complete");
      return okAsync(undefined);
    }

    // Extract the pass rate from the checks_succeeded line (format: "checks_succeeded...................: 100.00% 2 out of 2")
    const checkMatch = /([0-9.]+)%/.exec(checksLine);
    const checksPassRate = parseFloat(checkMatch?.[1] ?? "0");

    this.config.spinner.stop();
    logger.info(`Checks pass rate: ${checksPassRate}%`);

    if (errors.length > 0) {
      logger.error(`Errors: \n\n${errors.join("\n")}`);
    }
    if (warnings.length > 0) {
      logger.warn(`Warnings: \n\n${warnings.join("\n")}`);
    }

    if (checksPassRate < 100) {
      return errAsync({
        message: `k6 tests failed - ${checksPassRate}% checks passed`,
      });
    }

    this.config.spinner.succeed("Benchmark complete");
    return okAsync(undefined);
  }

  private buildK6EnvironmentVars(additionalVars?: Record<string, unknown>): string {
    const envVarMap = {
      API_KEY: this.config.apiKey,
      DURATION: this.config.duration,
      BATCH_SIZE: this.config.batchSize,
      COLLECTION_NAME: K6Benchmarks.COLLECTION_NAME,
      PORT: this.config.port,
      HOST: "host.docker.internal",
      COMMIT_HASH: this.config.commitHash,
      ...additionalVars,
    };

    return Object.entries(envVarMap)
      .map(([key, value]) => `-e ${key}=${value}`)
      .join(" ");
  }

  private createBenchmarkCollection(): ResultAsync<void, ErrorWithMessage> {
    this.config.spinner.start("Creating benchmark collection");

    const process = this.config.typesenseProcessManager.getProcessByHttpPort(this.config.port);
    if (!process.isOk()) {
      return errAsync(process.error);
    }

    const create = async () => {
      // Use a bounded request: the SDK client retries 503s for several minutes.
      const deadline = Date.now() + 60_000;
      let lastStatus = "no health response";
      let ready = false;
      while (!ready && Date.now() < deadline) {
        try {
          const response = await fetch(`http://localhost:${this.config.port}/health`, {
            signal: AbortSignal.timeout(Math.min(1_000, deadline - Date.now())),
          });
          lastStatus = `HTTP ${response.status}`;
          ready = response.ok && ((await response.json()) as { ok?: boolean }).ok === true;
        } catch (error) {
          lastStatus = toErrorWithMessage(error).message;
        }
        if (!ready) await new Promise((resolve) => setTimeout(resolve, 500));
      }
      if (!ready) {
        throw new Error(`Typesense on port ${this.config.port} not ready after 60s; last status: ${lastStatus}`);
      }

      const client = process.value.client;
      try {
        await client.collections().create(K6Benchmarks.COLLECTION_SCHEMA);
      } catch (error) {
        if (!(error instanceof Error) || !("httpStatus" in error) || error.httpStatus !== 409) throw error;
        const existing = await client.collections(K6Benchmarks.COLLECTION_NAME).retrieve();
        // CI uses a fresh data directory, so a conflict is our own retried create.
        if (existing.num_documents !== 0) {
          throw new Error("Benchmark collection songs already exists and is not empty; use a clean data directory");
        }
      }
    };

    return ResultAsync.fromPromise(create(), toErrorWithMessage).map(() => {
      this.config.spinner.succeed("Benchmark collection created");
    });
  }

  private getSearchBenchmarkPath(): ResultAsync<string, ErrorWithMessage> {
    return okAsync(path.join("/app", "src", "benchmarks", "search.ts"));
  }

  private getIndexingBenchmarkPath(): ResultAsync<string, ErrorWithMessage> {
    return okAsync(path.join("/app", "src", "benchmarks", "index.ts"));
  }
}
