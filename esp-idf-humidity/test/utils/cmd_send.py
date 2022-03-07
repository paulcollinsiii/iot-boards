#!/usr/bin/env python3

import contextlib
import asyncio
import logging
import uuid

import commands_pb2
from modules import ltr390_pb2, sht4x_pb2
from asyncio_mqtt import Client, ProtocolVersion


cmd_req_topic = "command/6dd576b8-8e70-4c8a-93d8-f2a2b98a4b9f/req/"
cmd_resp_topic = "command/6dd576b8-8e70-4c8a-93d8-f2a2b98a4b9f/resp/"

async def comm_stack():


    async with contextlib.AsyncExitStack() as stack:
        tasks = set()
        stack.push_async_callback(cancel_tasks, tasks)
        client = Client('mqtt.iot.kaffi.home', protocol=ProtocolVersion.V311)
        await stack.enter_async_context(client)

        manager = client.filtered_messages(cmd_resp_topic)
        messages = await stack.enter_async_context(manager)
        task = asyncio.create_task(log_cmd_result(messages))
        tasks.add(task)

        await client.subscribe(cmd_resp_topic)

        task = asyncio.create_task(post_cmds(client))
        tasks.add(task)

        await asyncio.gather(*tasks)


async def log_cmd_result(messages, count=8):
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
    cmd.alarm_add_request.crontab = '*/15 * * * * *'
    cmd.alarm_add_request.oneshot = True
    logging.info('cmd add_alarm: %s', cmd.SerializeToString())
    await client.publish(cmd_req_topic, payload=cmd.SerializeToString(), qos=2, retain=False)

    # Create a test alarm
    cmd = commands_pb2.CommandRequest()
    cmd.uuid = str(uuid.uuid4())
    cmd.alarm_add_request.crontab = '*/30 * * * * *'
    cmd.alarm_add_request.oneshot = True
    logging.info('cmd add_alarm: %s', cmd.SerializeToString())
    await client.publish(cmd_req_topic, payload=cmd.SerializeToString(), qos=2, retain=False)

    # List the alarms
    cmd = commands_pb2.CommandRequest()
    cmd.uuid = str(uuid.uuid4())
    cmd.alarm_list_request.SetInParent()
    logging.info('cmd list_alarms: %s', cmd.SerializeToString())
    await client.publish(cmd_req_topic, payload=cmd.SerializeToString(), qos=2, retain=False)

    # Delete the created alarm
    cmd = commands_pb2.CommandRequest()
    cmd.uuid = str(uuid.uuid4())
    cmd.alarm_delete_request.crontab = '*/15 * * * * *'
    logging.info('cmd delete_alarm: %s', cmd.SerializeToString())
    await client.publish(cmd_req_topic, payload=cmd.SerializeToString(), qos=2, retain=False)

    # List the alarms
    cmd = commands_pb2.CommandRequest()
    cmd.uuid = str(uuid.uuid4())
    cmd.alarm_list_request.SetInParent()
    logging.info('cmd list_alarms: %s', cmd.SerializeToString())
    await client.publish(cmd_req_topic, payload=cmd.SerializeToString(), qos=2, retain=False)

    logging.info('cmds send, exiting post_cmds')

    # GetLtr390 State
    cmd = commands_pb2.CommandRequest()
    cmd.uuid =str(uuid.uuid4())
    cmd.ltr390_get_options_request.SetInParent()
    logging.info('cmd ltr390 get options: %s', cmd.SerializeToString())
    await client.publish(cmd_req_topic, payload=cmd.SerializeToString(), qos=2, retain=False)

    # SetLtr390 Set UV mode
    cmd = commands_pb2.CommandRequest()
    cmd.uuid = str(uuid.uuid4())
    cmd.ltr390_set_options_request.SetInParent()
    cmd.ltr390_set_options_request.enable = True
    cmd.ltr390_set_options_request.mode = ltr390_pb2.UVS
    cmd.ltr390_set_options_request.gain = ltr390_pb2.GAIN_3
    cmd.ltr390_set_options_request.resolution = ltr390_pb2.RESOLUTION_18BIT
    cmd.ltr390_set_options_request.measurerate = ltr390_pb2.MEASURE_1000MS
    logging.info('cmd ltr390 set options: %s', cmd.SerializeToString())
    await client.publish(cmd_req_topic, payload=cmd.SerializeToString(), qos=2, retain=False)

    # Disable SHT4x
    cmd = commands_pb2.CommandRequest()
    cmd.uuid = str(uuid.uuid4())
    cmd.sht4x_set_options_request.SetInParent()
    cmd.sht4x_set_options_request.enable = False
    cmd.sht4x_set_options_request.mode = sht4x_pb2.NO_HEATER_HIGH
    logging.info('cmd sht4x set options: %s', cmd.SerializeToString())
    await client.publish(cmd_req_topic, payload=cmd.SerializeToString(), qos=2, retain=False)


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
