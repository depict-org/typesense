// Starts its own 3-node cluster (about 15 s, estimate) because the defect needs one member whose disk check differs from the leader's.
import { afterAll, beforeAll, describe, expect, it, setDefaultTimeout } from "bun:test";
import { mkdtempSync, rmSync } from "node:fs";
import { join } from "node:path";
import { Phases } from "../src/constants";
import { z } from "zod/v4";
import { TypesenseProcessManager } from "../src/manager";
import { fetchSingleNode } from "../src/request";

setDefaultTimeout(60000);
describe(Phases.NO_PHASE, () => {
  const ports = [28108, 28208, 28308];
  let manager: TypesenseProcessManager;
  let baseDir: string;
  let leader: number;
  const request = (port: number, path: string, init?: RequestInit) => fetchSingleNode(path, init, port);
  const collection = "/collections/committed_writes";
  const Status = z.object({ state: z.string(), committed_index: z.number(), queued_writes: z.number() });
  const status = async (port: number) => Status.parse(await (await request(port, "/status")).json());

  beforeAll(async () => {
    baseDir = mkdtempSync(join(process.env.TYPESENSE_DATA_DIR!, "committed-writes-"));
    manager = new TypesenseProcessManager(baseDir);
    manager.cleanDataDirs();
    await manager.startMultiNode(TypesenseProcessManager.multiNodeConfigs.map((node, i) => ({
      ...node, port: ports[i]!, peerPort: ports[i]! - 1,
      extraArgs: i === 2 ? ["--disk-used-max-percentage=1"] : [],
    })));
    const statuses = await Promise.all(ports.map(status));
    leader = ports[statuses.findIndex(status => status.state === "LEADER")]!;
    expect(leader).toBeDefined();
    expect(leader).not.toBe(ports[2]);
    expect((await request(leader, "/collections", {
      method: "POST", body: JSON.stringify({ name: "committed_writes", fields: [{ name: "title", type: "string" }] }),
    })).status).toBe(201);
    const imported = await request(leader, `${collection}/documents/import`, {
      method: "POST", body: [1, 2, 3].map(id => JSON.stringify({ id: String(id), title: `Document ${id}` })).join("\n"),
    });
    expect(imported.status).toBe(200);
    expect((await imported.text()).trim().split("\n").every(line => JSON.parse(line).success)).toBe(true);
    const deadline = Date.now() + 20000;
    while (true) {
      const head = await status(leader);
      const follower = await status(ports[2]!);
      if (follower.committed_index === head.committed_index && follower.queued_writes === 0) break;
      if (Date.now() > deadline) throw new Error(`Follower did not catch up: ${JSON.stringify(follower)}`);
      await Bun.sleep(100);
    }
  });

  afterAll(async () => {
    await manager?.shutdown();
    if (baseDir) rmSync(baseDir, { recursive: true, force: true });
  });

  it("applies committed writes on a follower above its disk admission limit", async () => {
    const res = await request(ports[2]!, collection);
    expect(res.status).toBe(200);
    expect(z.object({ num_documents: z.number() }).parse(await res.json()).num_documents).toBe(3);
    expect((await request(ports[2]!, `${collection}/documents/1`)).status).toBe(200);
  });

  it("still rejects writes admitted directly through that follower", async () => {
    const res = await request(ports[2]!, `${collection}/documents/import`, {
      method: "POST", body: JSON.stringify({ id: "rejected", title: "Must not enter Raft" }),
    });
    expect(res.status).toBe(422);
    for (const port of ports) expect((await request(port, `${collection}/documents/rejected`)).status).toBe(404);
  });
});
