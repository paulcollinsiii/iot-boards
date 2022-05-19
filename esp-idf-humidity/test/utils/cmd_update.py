#!/usr/bin/env python3

import contextlib
import asyncio
import logging
import uuid

import commands_pb2
from modules import ltr390_pb2, sht4x_pb2, blinky_pb2
from asyncio_mqtt import Client, ProtocolVersion


device_uuid = ["05474d0c-8e72-45de-8d32-a7dec3ec79bf", "0ea9ac26-d952-4094-bc0b-17622a788b0c"]
cmd_req_topic = [f"command/{did}/req/" for did in device_uuid]
cmd_resp_topic = [f"command/{did}/resp/" for did in device_uuid]

async def comm_stack():


    async with contextlib.AsyncExitStack() as stack:
        tasks = set()
        stack.push_async_callback(cancel_tasks, tasks)
        client = Client('mqtt.iot.kaffi.home', protocol=ProtocolVersion.V311)
        await stack.enter_async_context(client)

        for resp_topic in cmd_resp_topic:
            manager = client.filtered_messages(resp_topic)
            messages = await stack.enter_async_context(manager)
            task = asyncio.create_task(log_cmd_result(messages))
            tasks.add(task)

            await client.subscribe(resp_topic)

        task = asyncio.create_task(post_cmds(client))
        tasks.add(task)

        await asyncio.gather(*tasks)


async def log_cmd_result(messages, count=1):
    idx = 0
    async for message in messages:
        idx += 1
        payload = message.payload
        cmd_resp = commands_pb2.CommandResponse()
        logging.info(b'resp unparsed: "%s"', payload)
        cmd_resp.ParseFromString(payload)
        resp_type = cmd_resp.WhichOneof('resp')
        logging.info('resp parsed: UUID (%s) Ret (%s):(%s)', cmd_resp.uuid, cmd_resp.ret_code, resp_type)
        if resp_type == 'alarm_list_response':
            logging.info('resp alarms:\n%s', cmd_resp.alarm_list_response.alarms)
        if idx == count:
            return


async def post_cmds(client):
    # Create a test alarm
    cmd = commands_pb2.CommandRequest()
    cmd.uuid = str(uuid.uuid4())
    cmd.otamgr_update_request.SetInParent()

    logging.info('cmd update_request: %s', cmd.SerializeToString())
    await asyncio.gather(
        *[asyncio.create_task(client.publish(tpc, payload=cmd.SerializeToString(), qos=2, retain=False))
        for tpc in cmd_req_topic]
    )

async def cancel_tasks(tasks):
    for task in tasks:
        if task.done:
            continue
        task.cancel()
    try:
        await task
    except asyncio.CancelledError as e:
        logging.exception('Failed to cancel task, moving on...')


async def main():
    logging.info('Setting up Client')
    await comm_stack()

if __name__ == "__main__":
    logging.basicConfig(level='INFO')
    asyncio.run(main())
