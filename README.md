# Rate limiting client plugin

## Installation

Follow the [instructions](https://docs.halon.io/manual/comp_install.html#installation) in our manual to add our package repository and then run the below command.

### Ubuntu

```
apt-get install halon-extras-rate
```

### RHEL

```
yum install halon-extras-rate
```

## Configuration
For the configuration schemas, see [rate.schema.json](rate.schema.json) and [rate-app.schema.json](rate-app.schema.json).

## Exported functions

These functions needs to be [imported](https://docs.halon.io/hsl/structures.html#import) from the `extras://rate` module path.

### rate(namespace, entry, count, interval [, options])

Check or account for the rate of `entry` in `namespace` during the last `interval`. On error `none` is returned.

**Params**

- namespace `string` - The namespace
- entry `string` - The entry
- count `number` - The count
- interval `number` - The interval in seconds
- options `array` - Options array

The following options are available in the **options** array.

- sync `boolean` - Synchronize the rate in between nodes in the cluster. The default is `true`.

**Returns**

If `count` is greater than zero, it will increase the rate and return `true`, or return `false` if the limit is exceeded. If count is zero (`0`), it will return the number of items during the last `interval`. On error `none` is returned.

**Example**

```
import { rate } from "extras://rate";
if (rate("outbound", $connection["auth"]["username"], 3, 60) == false) {
      Reject("User is only allowed to send 3 messages per minute");
}
```