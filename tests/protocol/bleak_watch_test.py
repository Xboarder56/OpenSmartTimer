"""Exercise the actual watch helper without Bleak, a radio, or wall-clock waits."""
import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace

source = Path(__file__).resolve().parents[2] / 'examples/bleak_control.py'
function = next(n for n in ast.parse(source.read_text()).body
                if isinstance(n, ast.AsyncFunctionDef) and n.name == 'watch_connection')


async def check(disconnect=False):
    now = [0]
    reads = []
    client = SimpleNamespace(is_connected=True)

    async def sleep(seconds):
        assert seconds <= 60
        now[0] += seconds
        if disconnect and now[0] >= 120:
            client.is_connected = False

    async def read(uuid):
        reads.append(now[0])
        assert uuid == 'status'

    client.read_gatt_char = read
    namespace = {'BleakClient': object, 'LIVE_STATUS_UUID': 'status',
                 'asyncio': SimpleNamespace(sleep=sleep, get_running_loop=lambda:
                                           SimpleNamespace(time=lambda: now[0]))}
    exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), 'exec'), namespace)
    if disconnect:
        try:
            await namespace['watch_connection'](client, 601)
        except RuntimeError as error:
            assert 'disconnected' in str(error)
        else:
            raise AssertionError('disconnect was hidden')
    else:
        await namespace['watch_connection'](client, 601)
        assert reads == list(range(60, 601, 60))


asyncio.run(check())
asyncio.run(check(True))
print('Bleak watch keepalive and disconnect tests passed')
