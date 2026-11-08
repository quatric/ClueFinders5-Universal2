#import <Foundation/Foundation.h>
// Converts an NSArchiver typedstream to an XML plist
int main(int argc, const char **argv) {
	@autoreleasepool {
		NSData *d = [NSData dataWithContentsOfFile:@(argv[1])];
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		id obj = [NSUnarchiver unarchiveObjectWithData:d];
		NSError *err = nil;
		NSData *out = [NSPropertyListSerialization dataWithPropertyList:obj format:NSPropertyListXMLFormat_v1_0 options:0 error:&err];
		if (!out) { NSLog(@"%@ %@", err, [obj class]); return 1; }
		[out writeToFile:@(argv[2]) atomically:YES];
	}
	return 0;
}
