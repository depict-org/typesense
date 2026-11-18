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
  const Status = z.object({ state: z.string() });
  const status = async (port: number) => Status.parse(await (await request(port, "/status")).json());

  beforeAll(async () => {
    baseDir = mkdtempSync(join(process.env.TYPESENSE_DATA_DIR!, "committed-writes-"));
    manager = new TypesenseProcessManager(baseDir);
    manager.cleanDataDirs();
    await manager.startMultiNode(TypesenseProcessManager.multiNodeConfigs.map((node, nodeIndex) => ({
      ...node, port: ports[nodeIndex]!, peerPort: ports[nodeIndex]! - 1,
      extraArgs: nodeIndex === 2 ? ["--disk-used-max-percentage=1"] : [],
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
  });

  afterAll(async () => {
    await manager?.shutdown();
    if (baseDir) rmSync(baseDir, { recursive: true, force: true });
  });

  it("applies committed writes on a follower above its disk admission limit", async () => {
    const deadline = Date.now() + 2000;
    let res: Response;
    let numDocuments: number | undefined;
    do {
      res = await request(ports[2]!, collection);
      if (res.ok) numDocuments = z.object({ num_documents: z.number() }).parse(await res.json()).num_documents;
      if (numDocuments === 3) break;
      await Bun.sleep(50);
    } while (Date.now() < deadline);
    expect(res.status).toBe(200);
    expect(numDocuments).toBe(3);
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
