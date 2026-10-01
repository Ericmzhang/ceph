import { TestBed } from '@angular/core/testing';

import { CallHomeService } from './call-home.service';
import { HttpClientTestingModule } from '@angular/common/http/testing';
import { RouterTestingModule } from '@angular/router/testing';
import { configureTestBed } from '~/testing/unit-test-helper';
import { SharedModule } from '~/app/shared/shared.module';

describe('CallHomeService', () => {
  let service: CallHomeService;

  configureTestBed({
    imports: [HttpClientTestingModule, RouterTestingModule, SharedModule],
    providers: [CallHomeService]
  });

  beforeEach(() => {
    TestBed.configureTestingModule({});
    service = TestBed.inject(CallHomeService);
  });

  it('should be created', () => {
    expect(service).toBeTruthy();
  });
});
