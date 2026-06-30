"use strict";
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
exports.Browser = exports.Service = exports.Bonjour = void 0;
const bonjour_1 = __importStar(require("./lib/bonjour"));
Object.defineProperty(exports, "Service", { enumerable: true, get: function () { return bonjour_1.Service; } });
Object.defineProperty(exports, "Browser", { enumerable: true, get: function () { return bonjour_1.Browser; } });
class Bonjour extends bonjour_1.default {
}
exports.Bonjour = Bonjour;
exports.default = Bonjour;
module.exports = Object.assign(Bonjour, {
    Bonjour,
    Service: bonjour_1.Service,
    Browser: bonjour_1.Browser,
    default: Bonjour
});
//# sourceMappingURL=index.js.map