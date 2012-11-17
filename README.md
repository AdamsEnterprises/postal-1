# Postal

Postal is a push notification server for various mobile platforms.

## Requirements

 * Add device for notifications.
 * Remove device from notifications.
 * Query devices for a particular user id.
 * Deliver a notification to one or more devices.
 * Deliver a notification to all devices matching a query.

## Features

 * JSON API over HTTP.
 * Support for APS devices.
 * Support for C2DM devices.
 * Optimizations in message delivery to coalesce and aggregate when possible.
 * Doesn't require client side libraries, just use native platform libraries.

## Upcoming Features

 * Avoid duplicate sending of notifications with coallese key.
 * Support for GCM devices.
 * Queue notification delivery until the devices registered sleep time
   has elapsed.
 * Act as a MongoDB server for alternative failover scenarios.

## Important Notes

 * This server is meant to be installed behind your firewall. It does not
   perform authentication for your users. You do that in your API and then
   communicate with Postal internally.

## REST API

 * TODO: Describe REST API.