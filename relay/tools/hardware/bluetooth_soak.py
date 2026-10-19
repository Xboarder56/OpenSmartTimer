"""Short approved-key BLE phase for the mixed soak; preserves all approvals."""
import asyncio
import json
import struct
import time

import bluetooth_bench as bench_module
from bluetooth_bench import Bench, KEY, MGMT_RSP, ble
from bench_config import OUT

bench_module.REPORT = OUT / 'bluetooth-soak.json'


async def main():
    bench = Bench()
    try:
        initial = await bench.http('/api/v1/status')
        assert initial['state'] == 'open' and not initial['ota_armed']
        bench.report['identity'] = {k: initial[k] for k in ('device_id', 'boot_id', 'build_id')}
        target = await ble.find_timer('OST-' + initial['device_id'][-6:], None)
        async with ble.BleakClient(target) as bench.client:
            await ble.request_link_pairing(bench.client)
            bench.boot, bench.session_key = await ble.authenticate(bench.client, ble.read_key_file(KEY))
            bench.check('Existing approved key authenticates without new approval')
            await bench.client.start_notify(ble.CONTROL_RESULT_UUID,
                lambda _, data: bench.results.put_nowait(ble.decode_result(bytes(data))))
            await bench.client.start_notify(MGMT_RSP,
                lambda _, data: bench.ready.put_nowait(struct.unpack('<BBHHH', bytes(data))))
            for cycle in range(5):
                for opcode, path in ((0x44, '/api/v1/system/diagnostics'),
                                     (0x43, '/api/v1/bluetooth'), (0x47, '/api/v1/status')):
                    value = await bench.management(opcode)
                    rest = await bench.http(path)
                    assert set(value) == set(rest), 'BLE/REST field mismatch'
                    if opcode == 0x44:
                        assert value['sse_clients'] == 3, 'Three SSE viewers not retained during BLE'
                        bench.report['diagnostics'].append(value)
                    if opcode == 0x43:
                        assert value['approved_app_count'] == rest['approved_app_count']
                    if opcode == 0x47:
                        assert value['boot_id'] == initial['boot_id'] and value['build_id'] == initial['build_id']
                bench.check(f'Cycle {cycle + 1}: segmented authenticated management and REST parity')
                await bench.command('start', duration_ms=4000)
                await bench.command('pause')
                await bench.command('adjust', delta_ms=250)
                await bench.command('resume')
                await bench.command('release')
                assert (await bench.live())['state'] == 'open'
                await asyncio.sleep(7)
            bench.check('25 BLE commands coexist with three SSE viewers and REST load')
        final = await bench.http('/api/v1/status')
        assert final['state'] == 'open' and final['boot_id'] == initial['boot_id']
        bench.check('BLE disconnect preserves open timer and exact boot')
        bench.report['final_state'] = final['state']
    except Exception as error:
        bench.report['failures'].append(str(error))
        raise
    finally:
        bench.save()


if __name__ == '__main__':
    asyncio.run(main())
